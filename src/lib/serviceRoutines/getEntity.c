//
// FILE            getEntity.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                  // NULL
#include <stdlib.h>                                  // free
#include <string.h>                                  // strlen, strcpy, strcmp
#include <strings.h>                                 // strcasecmp
#include <stdint.h>                                  // int64_t, uint64_t
#include <time.h>                                    // clock_gettime
#include <regex.h>                                   // regexec

#include "kalloc/KAlloc.h"                           // KAlloc
#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjChildAdd, kjChildRemove
#include "kjson/kjChildReplace.h"                    // kjChildReplace
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjParse.h"                           // kjParse
#include "kjson/kjBufferCreate.h"                    // kjBufferCreate

#include "swRest/SwRestState.h"                      // swRest
#include "swJsonld/swldExpandTree.h"                 // swldExpandTree
#include "swJsonld/swldCompact.h"                    // swldCompact
#include "swJsonld/swldInit.h"                       // swldCoreContext
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldPickOmit
#include "swNgsild/LdVocab.h"                        // LD_VOCAB_*
#include "swNgsild/ldCheckDateTime.h"                // ldIsoToNanoseconds
#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieve
#include "swNgsild/LdForwarding.h"                   // LdForwardRequest, LdForwardResponse, LdForwardingPlugin
#include "swNgsild/ldForwarding.h"                   // ldForwardingForEndpoint
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant, ldViaHasAlias

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getEntity.h"               // Own interface



// -----------------------------------------------------------------------------
//
// strInList - true if NULL-terminated array v contains s
//
static bool strInList(const char* s, char** v)
{
  if (s == NULL || v == NULL)
    return false;

  for (int i = 0; v[i] != NULL; i++)
    if (strcmp(s, v[i]) == 0)
      return true;
  return false;
}



// -----------------------------------------------------------------------------
//
// stripInfoAttrsFromLocal - remove from localP every attr covered by one RegistrationInfo
//
// For exclusive/redirect: the broker cannot hold registered attrs locally.
// If the RegistrationInfo has no attr restriction (wildcard), ALL non-keyword
// attrs are stripped.
//
static void stripInfoAttrsFromLocal(KjNode* localP, LdRegInfo* riP)
{
  if (localP == NULL || localP->type != KjObject)
    return;

  bool wildcard = (riP->propertyNamesV == NULL && riP->relationshipNamesV == NULL);

  KjNode* curP = localP->value.firstChildP;
  while (curP != NULL)
  {
    KjNode* nextP = curP->next;

    if (curP->name != NULL && curP->name[0] != '@' &&
        strcmp(curP->name, "id")   != 0 &&
        strcmp(curP->name, "type") != 0 &&
        (wildcard ||
         strInList(curP->name, riP->propertyNamesV) ||
         strInList(curP->name, riP->relationshipNamesV)))
    {
      kjChildRemove(localP, curP);
    }

    curP = nextP;
  }
}



// -----------------------------------------------------------------------------
//
// nowNanoseconds - current time as epoch nanoseconds (for expiresAt checks)
//
static int64_t nowNanoseconds(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t) ts.tv_sec * 1000000000LL + (int64_t) ts.tv_nsec;
}



// -----------------------------------------------------------------------------
//
// instanceTsNanos - read a temporal field from an attribute instance as nanos
//
// Local instances have KjInt nanos (storage format); upstream-parsed instances
// have KjString ISO 8601 (sysAttrs=true rendering). Returns 0 if absent.
//
static int64_t instanceTsNanos(KjNode* instP, const char* fieldName)
{
  KjNode* p = kjLookup(instP, fieldName);
  if (p == NULL)
    return 0;
  if (p->type == KjInt)
    return p->value.i;
  if (p->type == KjString && p->value.s != NULL)
    return (int64_t) ldIsoToNanoseconds(p->value.s);
  return 0;
}



// -----------------------------------------------------------------------------
//
// instanceExpired - true if expiresAt is set and already in the past
//
static bool instanceExpired(KjNode* instP, int64_t nowNs)
{
  int64_t expires = instanceTsNanos(instP, LD_VOCAB_EXPIRES_AT);
  return (expires > 0 && expires <= nowNs);
}



// -----------------------------------------------------------------------------
//
// candidateBeats - § 4.5.5.3 conflict resolution between two attribute instances
//
// Returns true if 'cand' should replace 'cur' as the surviving instance:
//   - cur == NULL                                          → yes
//   - any non-expired with observedAt → newest observedAt wins
//   - else → newest modifiedAt wins
//
// Caller has already filtered out expired candidates.
//
static bool candidateBeats(KjNode* cand, KjNode* cur)
{
  if (cur == NULL)
    return true;

  int64_t candObs = instanceTsNanos(cand, LD_VOCAB_OBSERVED_AT);
  int64_t curObs  = instanceTsNanos(cur,  LD_VOCAB_OBSERVED_AT);

  if (candObs > 0 && curObs == 0)  return true;     // any observedAt beats none
  if (candObs == 0 && curObs > 0)  return false;
  if (candObs > 0 && curObs > 0)   return candObs > curObs;

  // neither has observedAt → fall back to modifiedAt
  int64_t candMod = instanceTsNanos(cand, LD_VOCAB_MODIFIED_AT);
  int64_t curMod  = instanceTsNanos(cur,  LD_VOCAB_MODIFIED_AT);
  return candMod > curMod;
}



// -----------------------------------------------------------------------------
//
// mergeOneSourceInto - apply § 4.5.5.3 to graft 'srcP' attrs into 'destP'
//
// Both trees are in storage format (each attribute is a wrapper of dataset-
// keyed instances). For every (attrName, dsKey) tuple present in srcP, we
// either install srcP's instance into destP (if no current or src wins per
// candidateBeats), or drop it. id/type/@-keywords are never touched on dest.
//
// Expired instances on either side are removed. No allocation: instances
// move (not clone) from srcP into destP, so srcP is left half-empty after.
//
static void mergeOneSourceInto(KjNode* destP, KjNode* srcP, int64_t nowNs)
{
  if (destP == NULL || srcP == NULL || srcP->type != KjObject)
    return;

  KjNode* srcAttrP = srcP->value.firstChildP;
  while (srcAttrP != NULL)
  {
    KjNode* nextSrcAttr = srcAttrP->next;

    if (srcAttrP->name == NULL || srcAttrP->name[0] == '@' ||
        strcmp(srcAttrP->name, "id")   == 0 ||
        strcmp(srcAttrP->name, "type") == 0 ||
        srcAttrP->type != KjObject)
    {
      srcAttrP = nextSrcAttr;
      continue;
    }

    KjNode* destAttrP = kjLookup(destP, srcAttrP->name);

    // Walk this src attr's instances (one per dsKey)
    KjNode* srcInstP = srcAttrP->value.firstChildP;
    while (srcInstP != NULL)
    {
      KjNode* nextSrcInst = srcInstP->next;

      if (instanceExpired(srcInstP, nowNs))
      {
        srcInstP = nextSrcInst;
        continue;
      }

      KjNode* destInstP = (destAttrP != NULL) ? kjLookup(destAttrP, srcInstP->name) : NULL;

      if (destInstP != NULL && instanceExpired(destInstP, nowNs))
      {
        kjChildRemove(destAttrP, destInstP);
        destInstP = NULL;
      }

      if (candidateBeats(srcInstP, destInstP))
      {
        // Detach srcInstP from src wrapper
        kjChildRemove(srcAttrP, srcInstP);
        srcInstP->next = NULL;

        if (destAttrP == NULL)
        {
          destAttrP = kjObject(swRest.kjsonP, srcAttrP->name);
          kjChildAdd(destP, destAttrP);
        }

        if (destInstP != NULL)
          kjChildRemove(destAttrP, destInstP);
        kjChildAdd(destAttrP, srcInstP);
      }

      srcInstP = nextSrcInst;
    }

    srcAttrP = nextSrcAttr;
  }
}



// -----------------------------------------------------------------------------
//
// mergeAuxiliaryInto - graft auxiliary source attrs into dest, fill gaps only
//
// Auxiliary registrations (§ 4.3.6.2) never override existing data. For every
// (attrName, dsKey) in srcP, add it to destP ONLY if destP doesn't already
// have that combination. No conflict resolution, no timestamp comparison.
//
static void mergeAuxiliaryInto(KjNode* destP, KjNode* srcP)
{
  if (destP == NULL || srcP == NULL || srcP->type != KjObject)
    return;

  KjNode* srcAttrP = srcP->value.firstChildP;
  while (srcAttrP != NULL)
  {
    KjNode* nextSrcAttr = srcAttrP->next;

    if (srcAttrP->name == NULL || srcAttrP->name[0] == '@' ||
        strcmp(srcAttrP->name, "id")   == 0 ||
        strcmp(srcAttrP->name, "type") == 0 ||
        srcAttrP->type != KjObject)
    {
      srcAttrP = nextSrcAttr;
      continue;
    }

    KjNode* destAttrP = kjLookup(destP, srcAttrP->name);

    KjNode* srcInstP = srcAttrP->value.firstChildP;
    while (srcInstP != NULL)
    {
      KjNode* nextSrcInst = srcInstP->next;

      // Only add if dest doesn't already have this (attrName, dsKey)
      KjNode* destInstP = (destAttrP != NULL) ? kjLookup(destAttrP, srcInstP->name) : NULL;
      if (destInstP != NULL)
      {
        srcInstP = nextSrcInst;
        continue;
      }

      // Detach from src
      kjChildRemove(srcAttrP, srcInstP);
      srcInstP->next = NULL;

      if (destAttrP == NULL)
      {
        destAttrP = kjObject(swRest.kjsonP, srcAttrP->name);
        kjChildAdd(destP, destAttrP);
      }
      kjChildAdd(destAttrP, srcInstP);

      srcInstP = nextSrcInst;
    }

    srcAttrP = nextSrcAttr;
  }
}



// -----------------------------------------------------------------------------
//
// apiAttrToStorageWrap - wrap upstream API-format entity into storage format
//
// Upstream gives us  "speed": { "type":"Property", "value":42 }
// Storage wants      "speed": { "@none": { "type":"Property", "value":42 } }
//
// We don't add timestamps (those are an authoritative-broker concept; for
// forwarded data we let the renderer's sysAttrs strip do its thing). We
// don't normalize value-keys either: upstream is parsed-then-expanded and
// "value" stays "value" (compaction would have renamed an LD_VOCAB_HAS_*
// IRI back to "value" anyway).
//
// Sub-attributes are not handled in this slice.
//
static void apiAttrToStorageWrap(KjNode* entityP, Kjson* kjP)
{
  if (entityP == NULL || entityP->type != KjObject)
    return;

  KjNode* curP = entityP->value.firstChildP;
  while (curP != NULL)
  {
    KjNode* nextP = curP->next;

    if (curP->name == NULL || curP->name[0] == '@' ||
        strcmp(curP->name, "id")   == 0 ||
        strcmp(curP->name, "type") == 0 ||
        curP->type != KjObject)
    {
      curP = nextP;
      continue;
    }

    KjNode*     dsP   = kjLookup(curP, LD_VOCAB_DATASET_ID);
    const char* dsKey = "@none";
    if (dsP != NULL)
    {
      dsKey = dsP->value.s;
      kjChildRemove(curP, dsP);
    }

    KjNode* wrapperP = kjObject(kjP, curP->name);
    kjChildReplace(entityP, curP, wrapperP);
    curP->name = (char*) dsKey;
    curP->next = NULL;
    kjChildAdd(wrapperP, curP);

    curP = nextP;
  }
}



// -----------------------------------------------------------------------------
//
// entityInfoMatchesId - does this EntityInfo cover the given entityId?
//
static bool entityInfoMatchesId(LdRegEntityInfo* eiP, const char* entityId)
{
  if (eiP->id != NULL)
    return (strcmp(eiP->id, entityId) == 0);

  if (eiP->idPatternList != NULL)
  {
    for (LdRegIdPattern* patP = eiP->idPatternList; patP != NULL; patP = patP->next)
      if (regexec(&patP->regex, entityId, 0, NULL, 0) == 0)
        return true;
    return false;
  }

  // No id and no pattern → matches any entity of this type
  return true;
}



// infoEntryMatchesEntity - does this RegistrationInfo entry match entityId?
//
static bool infoEntryMatchesEntity(LdRegInfo* riP, const char* entityId)
{
  for (LdRegEntityInfo* eiP = riP->entityInfoV; eiP != NULL; eiP = eiP->next)
    if (entityInfoMatchesId(eiP, entityId))
      return true;
  return false;
}



// -----------------------------------------------------------------------------
//
// forwardAndParse - forward retrieveEntity to a CSR and parse the upstream tree
//
// On success, *upstreamPP holds the expanded entity tree. On any HTTP
// failure (network error, non-2xx status, malformed body), returns the
// upstream's HTTP status (or 502 for transport errors) and *upstreamPP
// is set to NULL. Transport-level errors (no HTTP exchange) populate
// *errorDetailPP for the caller's diagnostic.
//
// buildInfoPickParam - build "&pick=a,b,c" from a single RegistrationInfo's attrs
//
// Returns "" if the RegistrationInfo has no attr restriction (wildcard).
//
static const char* buildInfoPickParam(LdRegInfo* riP, KAlloc* kaP)
{
  if (riP->propertyNamesV == NULL && riP->relationshipNamesV == NULL)
    return "";

  int totalLen = 0;
  int count    = 0;
  char** lists[] = { riP->propertyNamesV, riP->relationshipNamesV, NULL };

  for (int li = 0; lists[li] != NULL; li++)
    for (int i = 0; lists[li][i] != NULL; i++)
    {
      const char* c = swldCompact(swldCoreContext(), lists[li][i]);
      totalLen += strlen(c ? c : lists[li][i]) + 1;
      count++;
    }

  if (count == 0)
    return "";

  char* buf = (char*) kaAlloc(kaP, 6 + totalLen + 1);
  strcpy(buf, "&pick=");
  int pos = 6;

  for (int li = 0; lists[li] != NULL; li++)
    for (int i = 0; lists[li][i] != NULL; i++)
    {
      if (pos > 6) buf[pos++] = ',';
      const char* c = swldCompact(swldCoreContext(), lists[li][i]);
      const char* n = c ? c : lists[li][i];
      int nlen = strlen(n);
      strcpy(buf + pos, n);
      pos += nlen;
    }

  buf[pos] = 0;
  return buf;
}



// forwardAndParse - forward retrieveEntity for ONE RegistrationInfo entry
//
// Each RegistrationInfo within a CSR is a separate dispatch unit — its own
// (type, attr-set) coverage area. The forwarded URL is constrained to that
// entry's attrs via &pick= and optionally &type= from the matching EntityInfo.
//
static int forwardAndParse(LdRegCacheItem* csr,
                           LdRegInfo*       riP,
                           const char*      entityId,
                           const char*      ownAlias,
                           KjNode**         upstreamPP,
                           const char**     errorDetailPP)
{
  *upstreamPP    = NULL;
  *errorDetailPP = NULL;

  const LdForwardingPlugin* plugin = ldForwardingForEndpoint(csr->endpoint);
  if (plugin == NULL)
  {
    *errorDetailPP = "no forwarding plugin available for endpoint";
    return 502;
  }

  // Build URL: <endpoint>/entities/<entityId>?sysAttrs=true[&type=X][&pick=a,b,c]
  const char* base = csr->endpoint;
  const char* path = "/entities/";
  const char* qs   = "?sysAttrs=true";
  const char* pick = buildInfoPickParam(riP, &swRest.kalloc);

  // Type constraint: use the EntityInfo's type that matched (not the client's
  // ?type= param) — this is what the registration covers.
  const char* typePart = "";
  if (riP->entityInfoV != NULL && riP->entityInfoV->type != NULL)
  {
    const char* ct = swldCompact(swldCoreContext(), riP->entityInfoV->type);
    const char* tn = ct ? ct : riP->entityInfoV->type;
    int tLen = strlen(tn);
    char* tp = (char*) kaAlloc(&swRest.kalloc, 6 + tLen + 1);
    strcpy(tp, "&type=");
    strcpy(tp + 6, tn);
    typePart = tp;
  }

  int baseLen = strlen(base);
  int pathLen = strlen(path);
  int idLen   = strlen(entityId);
  int qsLen   = strlen(qs);
  int pickLen = strlen(pick);
  int typeLen = strlen(typePart);
  char* url   = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + idLen + qsLen + typeLen + pickLen + 1);

  strcpy(url, base);
  strcpy(url + baseLen, path);
  strcpy(url + baseLen + pathLen, entityId);
  strcpy(url + baseLen + pathLen + idLen, qs);
  strcpy(url + baseLen + pathLen + idLen + qsLen, typePart);
  strcpy(url + baseLen + pathLen + idLen + qsLen + typeLen, pick);

  // Build outbound headers: pass through any incoming Via headers (so the
  // chain stays observable end-to-end) plus our own per-tenant alias as a
  // new Via entry. RFC 7230 § 5.7.1: Via is a list-valued header; one entry
  // per hop, comma-separated. We keep one SwRestKeyValue per hop instead
  // of joining — swRest's downstream serializer concatenates list values.
  //
  SwRestKeyValue* hv = NULL;
  int             hc = 0;

  int viaIn = 0;
  for (int i = 0; i < swRest.in.httpHeaderCount; i++)
    if (swRest.in.httpHeaderV[i].key != NULL && strcasecmp(swRest.in.httpHeaderV[i].key, "Via") == 0)
      viaIn++;

  if (ownAlias != NULL || viaIn > 0)
  {
    hv = (SwRestKeyValue*) kaAlloc(&swRest.kalloc, (viaIn + 1) * sizeof(SwRestKeyValue));
    for (int i = 0; i < swRest.in.httpHeaderCount; i++)
    {
      if (swRest.in.httpHeaderV[i].key != NULL && strcasecmp(swRest.in.httpHeaderV[i].key, "Via") == 0)
      {
        hv[hc].key   = (char*) "Via";
        hv[hc].value = swRest.in.httpHeaderV[i].value;
        hc++;
      }
    }
    if (ownAlias != NULL)
    {
      int   aliasLen = strlen(ownAlias);
      char* viaVal   = (char*) kaAlloc(&swRest.kalloc, 4 + aliasLen + 1);  // "1.1 " + alias
      strcpy(viaVal, "1.1 ");
      strcpy(viaVal + 4, ownAlias);
      hv[hc].key   = (char*) "Via";
      hv[hc].value = viaVal;
      hc++;
    }
  }

  LdForwardRequest  req;
  LdForwardResponse resp;

  req.endpoint         = url;
  req.verb             = SwVerbGet;
  req.headerV          = hv;
  req.headerCount      = hc;
  req.body             = NULL;
  req.bodyLen          = 0;
  req.connectTimeoutMs = 0;
  req.requestTimeoutMs = 0;

  resp.statusCode      = 0;
  resp.headerV         = NULL;
  resp.headerCount     = 0;
  resp.body            = NULL;
  resp.bodyLen         = 0;
  resp.allocP          = &swRest.kalloc;
  resp.error           = 0;
  resp.errorDetail[0]  = 0;

  int rc = plugin->send(&req, &resp);
  if (rc != 0)
  {
    if (resp.errorDetail[0] != 0)
    {
      char* d = (char*) kaAlloc(&swRest.kalloc, strlen(resp.errorDetail) + 1);
      strcpy(d, resp.errorDetail);
      *errorDetailPP = d;
    }
    return 502;
  }

  if (resp.statusCode < 200 || resp.statusCode >= 300)
    return resp.statusCode;

  if (resp.body == NULL || resp.bodyLen == 0)
  {
    *errorDetailPP = "empty body in upstream 2xx response";
    return 502;
  }

  KjNode* treeP = kjParse(swRest.kjsonP, resp.body);
  if (treeP == NULL)
  {
    *errorDetailPP = "upstream returned malformed JSON";
    return 502;
  }

  swldExpandTree(treeP, &swRest.kalloc);

  // Convert upstream's API form into the storage format the renderHook
  // (ldEntityToApi) expects, so the merge result can flow through the
  // normal output pipeline.
  apiAttrToStorageWrap(treeP, swRest.kjsonP);

  *upstreamPP = treeP;
  return resp.statusCode;
}



// -----------------------------------------------------------------------------
//
// getEntity -
//
bool getEntity(void)
{
  const char* entityId = swRest.in.wildcard[0];

  //
  // DistOps dispatch (§ 5.7.1.4): always attempt local AND forward to
  // any matching registrations. Processing order per § 4.3.6:
  //   1. Exclusive: strip local registered attrs, forward (single source)
  //   2. Redirect:  strip local registered attrs, forward each, merge
  //                 (multiple redirects may coexist, unlike exclusive)
  //   3. Inclusive:  forward each, merge per § 4.5.5.3
  //   4. Auxiliary:  forward each, fill gaps only (never overrides)
  // ?local=true (§ 5.5.13) bypasses the dispatcher entirely.
  //
  if (swNgsild.local == false)
  {
    Tenant*           tP        = (Tenant*) swNgsild.tenantP;
    LdRegCacheItem**  exclV     = NULL;
    LdRegCacheItem**  redirV    = NULL;
    LdRegCacheItem**  inclV     = NULL;
    LdRegCacheItem**  auxV      = NULL;
    int               exclN     = 0;
    int               redirN    = 0;
    int               inclN     = 0;
    int               auxN      = 0;

    const char* entityType = swNgsild.type;   // expanded in preServiceHook

    if (tP != NULL && tP->regCacheP != NULL)
    {
      exclN  = ldRegCacheMatchForRetrieve((LdRegCache*) tP->regCacheP,
                                          entityId, entityType,
                                          LdRegModeExclusive, &exclV);
      redirN = ldRegCacheMatchForRetrieve((LdRegCache*) tP->regCacheP,
                                          entityId, entityType,
                                          LdRegModeRedirect, &redirV);
      inclN  = ldRegCacheMatchForRetrieve((LdRegCache*) tP->regCacheP,
                                          entityId, entityType,
                                          LdRegModeInclusive, &inclV);
      auxN   = ldRegCacheMatchForRetrieve((LdRegCache*) tP->regCacheP,
                                          entityId, NULL,
                                          LdRegModeAuxiliary, &auxV);
    }

    if (exclN > 0 || redirN > 0 || inclN > 0 || auxN > 0)
    {
      // Loop detection runs once per request (per § 5.7.5 / RFC 7230)
      const char* ownAlias = ldCsourceAliasForTenant(tP != NULL ? tP->name : NULL,
                                                      &swRest.kalloc);
      if (ownAlias != NULL && ldViaHasAlias(swRest.in.httpHeaderV, swRest.in.httpHeaderCount, ownAlias))
      {
        ldError(502, LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                "loop detected: own alias '%s' present in incoming Via header", ownAlias);
        if (exclV  != NULL) free(exclV);
        if (redirV != NULL) free(redirV);
        if (inclV  != NULL) free(inclV);
        if (auxV   != NULL) free(auxV);
        return true;
      }

      // Local lookup (always — per § 5.7.1.4)
      KjNode* destP = NULL;
      db.entityRetrieve(tP, entityId, &destP);

      int64_t nowNs = nowNanoseconds();

      //
      // Per-RegistrationInfo dispatch. Each RegistrationInfo within a CSR
      // is a separate (type, attr-set) coverage unit. One forward per
      // matching RegistrationInfo, constrained by that entry's attrs and
      // type. This avoids overquerying (§ 4.3.6.1).
      //

      // --- Exclusive (§ 4.3.6.3): strip local + forward, single source ---
      for (int i = 0; i < exclN; i++)
      {
        LdRegCacheItem* csr = exclV[i];
        if (csr->endpoint == NULL)
        {
          ldError(502, LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                  "exclusive registration '%s' has no endpoint", csr->regId);
          if (exclV  != NULL) free(exclV);
          if (redirV != NULL) free(redirV);
          if (inclV  != NULL) free(inclV);
          if (auxV   != NULL) free(auxV);
          return true;
        }
        for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
        {
          if (!infoEntryMatchesEntity(riP, entityId))
            continue;
          if (destP != NULL)
            stripInfoAttrsFromLocal(destP, riP);
          KjNode*     upP    = NULL;
          const char* upErr  = NULL;
          int         upCode = forwardAndParse(csr, riP, entityId, ownAlias, &upP, &upErr);
          if (upCode == 404) continue;
          if (upCode < 200 || upCode >= 300)
          {
            if (upErr) ldError(502, LD_ERROR_INTERNAL_ERROR, "Bad Gateway", "forwarded request failed: %s", upErr);
            else       ldError(502, LD_ERROR_INTERNAL_ERROR, "Bad Gateway", "forwarded request failed (status %d)", upCode);
            if (exclV  != NULL) free(exclV);
            if (redirV != NULL) free(redirV);
            if (inclV  != NULL) free(inclV);
            if (auxV   != NULL) free(auxV);
            return true;
          }
          if (destP == NULL) destP = upP;
          else               mergeOneSourceInto(destP, upP, nowNs);
        }
      }

      // --- Redirect (§ 4.3.6.3): strip local + forward each, merge ---
      for (int i = 0; i < redirN; i++)
      {
        LdRegCacheItem* csr = redirV[i];
        if (csr->endpoint == NULL) continue;
        for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
        {
          if (!infoEntryMatchesEntity(riP, entityId))
            continue;
          if (destP != NULL)
            stripInfoAttrsFromLocal(destP, riP);
          KjNode*     upP    = NULL;
          const char* upErr  = NULL;
          int         upCode = forwardAndParse(csr, riP, entityId, ownAlias, &upP, &upErr);
          if (upCode < 200 || upCode >= 300 || upP == NULL) continue;
          if (destP == NULL) destP = upP;
          else               mergeOneSourceInto(destP, upP, nowNs);
        }
      }

      // --- Inclusive: forward each, merge per § 4.5.5.3 ---
      for (int i = 0; i < inclN; i++)
      {
        LdRegCacheItem* csr = inclV[i];
        if (csr->endpoint == NULL) continue;
        for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
        {
          if (!infoEntryMatchesEntity(riP, entityId))
            continue;
          KjNode*     upP    = NULL;
          const char* upErr  = NULL;
          int         upCode = forwardAndParse(csr, riP, entityId, ownAlias, &upP, &upErr);
          (void) upErr;
          if (upCode < 200 || upCode >= 300 || upP == NULL) continue;
          if (destP == NULL) destP = upP;
          else               mergeOneSourceInto(destP, upP, nowNs);
        }
      }

      // --- Auxiliary (§ 4.3.6.2): fill gaps only, never overrides ---
      for (int i = 0; i < auxN; i++)
      {
        LdRegCacheItem* csr = auxV[i];
        if (csr->endpoint == NULL) continue;
        for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
        {
          if (!infoEntryMatchesEntity(riP, entityId))
            continue;
          KjNode*     upP    = NULL;
          const char* upErr  = NULL;
          int         upCode = forwardAndParse(csr, riP, entityId, ownAlias, &upP, &upErr);
          (void) upErr;
          if (upCode < 200 || upCode >= 300 || upP == NULL) continue;
          if (destP == NULL) destP = upP;
          else               mergeAuxiliaryInto(destP, upP);
        }
      }

      if (exclV  != NULL) free(exclV);
      if (redirV != NULL) free(redirV);
      if (inclV  != NULL) free(inclV);
      if (auxV   != NULL) free(auxV);

      if (destP == NULL)
      {
        ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                "entity '%s' not found", entityId);
        return true;
      }

      if (swNgsild.pickV != NULL || swNgsild.omitV != NULL)
        ldPickOmit(destP, swNgsild.pickV, swNgsild.omitV);
      swRest.out.responseTree = destP;
      return true;
    }
  }

  KjNode* entityP = NULL;
  int     r       = db.entityRetrieve((Tenant*) swNgsild.tenantP, entityId, &entityP);

  if (r == DB_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity '%s' not found", entityId);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error retrieving entity '%s'", entityId);
    return true;
  }

  // Apply pick/omit attribute projection
  if (swNgsild.pickV != NULL || swNgsild.omitV != NULL)
    ldPickOmit(entityP, swNgsild.pickV, swNgsild.omitV);

  swRest.out.responseTree = entityP;
  return true;
}

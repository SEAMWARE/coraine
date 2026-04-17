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
// nameInAnyInfo - is attribute IRI 'name' covered by any RegistrationInfo of csr?
//
static bool nameInAnyInfo(LdRegCacheItem* csr, const char* name)
{
  for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
  {
    if (strInList(name, riP->propertyNamesV))     return true;
    if (strInList(name, riP->relationshipNamesV)) return true;
  }
  return false;
}



// -----------------------------------------------------------------------------
//
// stripCoveredAttrs - remove from localP every attr covered by csr
//
// Spec § 4.3.6.3: an exclusive registration forbids the broker from holding
// the registered attrs locally. If legacy data violates this, defensive: we
// drop those attrs from the local entity before merging upstream's view.
//
static void stripCoveredAttrs(KjNode* localP, LdRegCacheItem* csr)
{
  if (localP == NULL || localP->type != KjObject)
    return;

  KjNode* curP = localP->value.firstChildP;
  while (curP != NULL)
  {
    KjNode* nextP = curP->next;

    if (curP->name != NULL && curP->name[0] != '@' &&
        strcmp(curP->name, "id")   != 0 &&
        strcmp(curP->name, "type") != 0 &&
        nameInAnyInfo(csr, curP->name))
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
// forwardAndParse - forward retrieveEntity to a CSR and parse the upstream tree
//
// On success, *upstreamPP holds the expanded entity tree. On any HTTP
// failure (network error, non-2xx status, malformed body), returns the
// upstream's HTTP status (or 502 for transport errors) and *upstreamPP
// is set to NULL. Transport-level errors (no HTTP exchange) populate
// *errorDetailPP for the caller's diagnostic.
//
static int forwardAndParse(LdRegCacheItem* csr,
                           const char*     entityId,
                           const char*     ownAlias,
                           KjNode**        upstreamPP,
                           const char**    errorDetailPP)
{
  *upstreamPP    = NULL;
  *errorDetailPP = NULL;

  const LdForwardingPlugin* plugin = ldForwardingForEndpoint(csr->endpoint);
  if (plugin == NULL)
  {
    *errorDetailPP = "no forwarding plugin available for endpoint";
    return 502;
  }

  const char* base    = csr->endpoint;
  const char* path    = "/entities/";
  const char* qs      = "?sysAttrs=true";       // need timestamps for §4.5.5.3 merge
  int         baseLen = strlen(base);
  int         pathLen = strlen(path);
  int         idLen   = strlen(entityId);
  int         qsLen   = strlen(qs);
  char*       url     = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + idLen + qsLen + 1);

  strcpy(url, base);
  strcpy(url + baseLen, path);
  strcpy(url + baseLen + pathLen, entityId);
  strcpy(url + baseLen + pathLen + idLen, qs);

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
  // DistOps dispatch (§ 5.7.1.4): always attempt local AND, if any
  // exclusive or inclusive registration matches, forward + merge per
  // § 4.5.5.3. ?local=true (§ 5.5.13) bypasses the dispatcher entirely.
  // Auxiliary mode is retrieve-only and not handled in this slice;
  // redirect is treated as exclusive for retrieve purposes (single
  // authoritative upstream).
  //
  if (swNgsild.local == false)
  {
    Tenant*           tP        = (Tenant*) swNgsild.tenantP;
    LdRegCacheItem**  exclV     = NULL;
    LdRegCacheItem**  inclV     = NULL;
    int               exclN     = 0;
    int               inclN     = 0;

    if (tP != NULL && tP->regCacheP != NULL)
    {
      exclN = ldRegCacheMatchForRetrieve((LdRegCache*) tP->regCacheP,
                                         entityId, NULL,
                                         LdRegModeExclusive, &exclV);
      inclN = ldRegCacheMatchForRetrieve((LdRegCache*) tP->regCacheP,
                                         entityId, NULL,
                                         LdRegModeInclusive, &inclV);
    }

    if (exclN > 0 || inclN > 0)
    {
      // Loop detection runs once per request (per § 5.7.5 / RFC 7230)
      const char* ownAlias = ldCsourceAliasForTenant(tP != NULL ? tP->name : NULL,
                                                      &swRest.kalloc);
      if (ownAlias != NULL && ldViaHasAlias(swRest.in.httpHeaderV, swRest.in.httpHeaderCount, ownAlias))
      {
        ldError(502, LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                "loop detected: own alias '%s' present in incoming Via header", ownAlias);
        if (exclV != NULL) free(exclV);
        if (inclV != NULL) free(inclV);
        return true;
      }

      // Local lookup (always — per § 5.7.1.4)
      KjNode* destP = NULL;
      db.entityRetrieve(tP, entityId, &destP);

      int64_t nowNs = nowNanoseconds();

      //
      // Exclusive sources: defensive strip of registered attrs from local
      // (§ 4.3.6.3), then forward + merge (no §4.5.5.3 conflict needed since
      // dest's slots for those attrs are now empty).
      //
      for (int i = 0; i < exclN; i++)
      {
        LdRegCacheItem* csr = exclV[i];

        if (csr->endpoint == NULL)
        {
          ldError(502, LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                  "exclusive registration '%s' has no endpoint", csr->regId);
          if (exclV != NULL) free(exclV);
          if (inclV != NULL) free(inclV);
          return true;
        }

        if (destP != NULL)
          stripCoveredAttrs(destP, csr);

        KjNode*     upP    = NULL;
        const char* upErr  = NULL;
        int         upCode = forwardAndParse(csr, entityId, ownAlias, &upP, &upErr);

        if (upCode == 404)
          continue;
        if (upCode < 200 || upCode >= 300)
        {
          if (upErr != NULL)
            ldError(502, LD_ERROR_INTERNAL_ERROR, "Bad Gateway", "forwarded request failed: %s", upErr);
          else
            ldError(502, LD_ERROR_INTERNAL_ERROR, "Bad Gateway", "forwarded request failed (status %d)", upCode);
          if (exclV != NULL) free(exclV);
          if (inclV != NULL) free(inclV);
          return true;
        }

        if (destP == NULL)
          destP = upP;
        else
          mergeOneSourceInto(destP, upP, nowNs);
      }

      //
      // Inclusive sources: forward + merge per § 4.5.5.3. Per-source
      // failures are tolerated (partial-success) — caller still gets the
      // surviving sources' contribution.
      //
      for (int i = 0; i < inclN; i++)
      {
        LdRegCacheItem* csr = inclV[i];

        if (csr->endpoint == NULL)
          continue;

        KjNode*     upP    = NULL;
        const char* upErr  = NULL;
        int         upCode = forwardAndParse(csr, entityId, ownAlias, &upP, &upErr);
        (void) upErr;

        if (upCode < 200 || upCode >= 300 || upP == NULL)
          continue;

        if (destP == NULL)
          destP = upP;
        else
          mergeOneSourceInto(destP, upP, nowNs);
      }

      if (exclV != NULL) free(exclV);
      if (inclV != NULL) free(inclV);

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

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
#include "kbase/kStringInArray.h"                    // kStringInArray
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjChildAdd, kjChildRemove
#include "kjson/kjChildReplace.h"                    // kjChildReplace
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjParse.h"                           // kjParse
#include "kjson/kjBufferCreate.h"                    // kjBufferCreate

#include "swRest/SwRestState.h"                      // swRest
#include "swJsonld/swldExpandTree.h"                 // swldExpandTree
#include "swJsonld/swldExpand.h"                     // swldExpand
#include "swJsonld/swldCompact.h"                    // swldCompact
#include "swJsonld/swldInit.h"                       // swldCoreContext
#include "swJsonld/swldDownload.h"                   // swldContextFromUrl
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldPickOmit
#include "swNgsild/ldParamsValidate.h"               // ldParamsValidate
#include "swNgsild/ldAcceptParse.h"                  // ldAcceptParse, LdAcceptGeoJson
#include "swNgsild/LdVocab.h"                        // LD_VOCAB_*
#include "swNgsild/ldStripAtContext.h"              // ldStripAtContext
#include "swNgsild/ldExpiresAtPropagate.h"          // ldExpiresAtPropagate
#include "swNgsild/ldCheckDateTime.h"                // ldIsoToNanoseconds
#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieve
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant, ldViaHasAlias
#include "swNgsild/ldDistOp.h"                       // ldDistOpLoopDetected, ldDistOpSendReceive, ldDistOpForwardContext
#include "swNgsild/ldQRender.h"                      // ldCompactOrEncode

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "linkedEntities/ldLinkedEntities.h"         // ldLinkedEntitiesFlat

#include "serviceRoutines/ldSnapshotRead.h"          // ldSnapshotItemFromHeader, snapshotGetEntity
#include "serviceRoutines/getEntity.h"               // Own interface



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

  bool wildcard = (riP->attributeNamesV == NULL);

  KjNode* curP = localP->value.firstChildP;
  while (curP != NULL)
  {
    KjNode* nextP = curP->next;

    if (curP->name != NULL && curP->name[0] != '@' &&
        strcmp(curP->name, "id")   != 0 &&
        strcmp(curP->name, "type") != 0 &&
        (wildcard ||
         kStringInArray(curP->name, riP->attributeNamesV)))
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
// userWantsField - does the user's pick/omit allow `name` to survive?
//
// Pick wins if present (only listed members survive). Omit wins if pick
// is absent (everything except listed members survives). Both absent
// means no user restriction → everything survives.
//
static bool userWantsField(const char* name)
{
  if (swNgsild.pickV != NULL)
  {
    for (int i = 0; swNgsild.pickV[i] != NULL; i++)
      if (strcmp(swNgsild.pickV[i], name) == 0)
        return true;
    return false;
  }
  if (swNgsild.omitV != NULL)
  {
    for (int i = 0; swNgsild.omitV[i] != NULL; i++)
      if (strcmp(swNgsild.omitV[i], name) == 0)
        return false;
    return true;
  }
  return true;
}



// buildInfoPickParam - build "&pick=type,scope,a,b,c" from a single RegistrationInfo's attrs
//
// Returns "" if the RegistrationInfo has no attr restriction (wildcard)
// — the CS will return the full entity, no projection needed.
//
// `type` and `scope` are entity-level members that the spec (§ 6.3.4)
// requires on every entity representation (type) or that can be
// distributed across multiple Context Sources (scope). The broker
// cannot reconstruct either from its own knowledge:
//   - type: an entity can carry multiple types; the CSR only pins the
//     one used for matching.
//   - scope: a part of the entity held in another CS may carry a
//     different slice of the total scope, and the merge has to add them
//     together (no-split retrieve aggregates from every claiming CS).
// Both are included in the forward `pick` unless the user's request
// would have stripped them anyway (explicit `omit` or a `pick` that
// doesn't list them) — no point fetching what we'll throw away.
//
// `id` is NOT included — the broker knows the id (it's the URL it
// forwarded to) and reinjects it via ensureEntityId on the way back.
//
static const char* buildInfoPickParam(LdRegCacheItem* csr, LdRegInfo* riP, KAlloc* kaP, bool forceTypeScope)
{
  if (riP->attributeNamesV == NULL)
    return "";

  // forceTypeScope: a whole-entity fetch (e.g. a linked-entity assemble)
  // always needs type + scope back so the merge is complete — it has no
  // request pick/omit to consult. The request-driven retrieve consults
  // userWantsField so it doesn't fetch members it would strip anyway.
  bool wantType  = forceTypeScope || userWantsField("type");
  bool wantScope = forceTypeScope || userWantsField("scope");

  // The CS will interpret the URL params via the same @context that
  // accompanies the forward (Link header for application/json, body
  // @context for application/ld+json). Both resolve to
  // ldDistOpForwardContext(csr) — csi.jsonldContext > incoming request
  // context > core. Compacting against any other context would emit
  // short names the CS expands differently than we intended; IRIs with
  // no short form there are %-encoded (ldCompactOrEncode).
  SwldContext* forwardCtx = (csr != NULL) ? ldDistOpForwardContext(csr) : swldCoreContext();

  int totalLen = 0;
  int count    = 0;

  for (int i = 0; riP->attributeNamesV[i] != NULL; i++)
  {
    totalLen += strlen(ldCompactOrEncode(riP->attributeNamesV[i], forwardCtx, kaP, false)) + 1;
    count++;
  }

  if (count == 0)
    return "";

  // Header room: "&pick=" + "type," (5) + "scope," (6) = up to 17 chars.
  char* buf = (char*) kaAlloc(kaP, 6 + 11 + totalLen + 1);
  strcpy(buf, "&pick=");
  int pos = 6;

  if (wantType)  { memcpy(buf + pos, "type", 4);  pos += 4; buf[pos++] = ','; }
  if (wantScope) { memcpy(buf + pos, "scope", 5); pos += 5; buf[pos++] = ','; }

  // After the type/scope prefix `pos` is just past the trailing comma
  // (or still at 6 if neither was included). Either way, the first attr
  // writes directly; subsequent attrs prepend a comma.
  int firstAttrPos = pos;

  for (int i = 0; riP->attributeNamesV[i] != NULL; i++)
  {
    if (pos > firstAttrPos) buf[pos++] = ',';
    const char* n = ldCompactOrEncode(riP->attributeNamesV[i], forwardCtx, kaP, false);
    int nlen = strlen(n);
    strcpy(buf + pos, n);
    pos += nlen;
  }

  buf[pos] = 0;
  return buf;
}



// buildQueryFormUrl - § 9.2 ops-aware conversion: the CSR supports
// queryEntity but not retrieveEntity, so the by-id retrieve forwards as
// GET /entities?id=<entityId> (+ sysAttrs for the merge + the
// RegistrationInfo's pick narrowing, same as the by-id form). No type=
// — the registration's types are expanded IRIs that need not compact
// for the receiver, and the pinned id alone identifies the entity.
//
static char* buildQueryFormUrl(LdRegCacheItem* csr, LdRegInfo* riP, const char* entityId, bool forceTypeScope)
{
  const char* base    = csr->endpoint;
  const char* path    = "/ngsi-ld/v1/entities?id=";
  const char* qs      = "&sysAttrs=true";
  const char* pickRaw = buildInfoPickParam(csr, riP, &swRest.kalloc, forceTypeScope);

  int baseLen = strlen(base);
  int pathLen = strlen(path);
  int idLen   = strlen(entityId);
  int qsLen   = strlen(qs);
  int pickLen = strlen(pickRaw);
  char* url   = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + idLen + qsLen + pickLen + 1);

  char* p = url;
  memcpy(p, base, baseLen);       p += baseLen;
  memcpy(p, path, pathLen);       p += pathLen;
  memcpy(p, entityId, idLen);     p += idLen;
  memcpy(p, qs, qsLen);           p += qsLen;
  memcpy(p, pickRaw, pickLen);    p += pickLen;
  *p = 0;
  return url;
}



// buildQueryBatchForm - § 9.2 ops-aware conversion: queryBatch-only CSR.
// The retrieve forwards as POST /entityOperations/query with a § 5.2.23
// Query body selecting the one entity id; attribute narrowing rides in
// the body's "attrs" (mirroring the GET form's pick).
//
static void buildQueryBatchForm(LdRegCacheItem* csr, LdRegInfo* riP, const char* entityId,
                                LdDistOpBatchItem* itemP)
{
  const char* path    = "/ngsi-ld/v1/entityOperations/query?sysAttrs=true";
  int   baseLen = strlen(csr->endpoint);
  char* url     = (char*) kaAlloc(&swRest.kalloc, baseLen + strlen(path) + 1);
  strcpy(url, csr->endpoint);
  strcpy(url + baseLen, path);

  // Body: {"type":"Query","entities":[{"id":"<entityId>"}]}
  static const char bodyPre[]  = "{\"type\":\"Query\",\"entities\":[{\"id\":\"";
  static const char bodyPost[] = "\"}]}";
  int   idLen = strlen(entityId);
  char* body  = (char*) kaAlloc(&swRest.kalloc, sizeof(bodyPre) - 1 + idLen + sizeof(bodyPost));
  char* p     = body;
  memcpy(p, bodyPre, sizeof(bodyPre) - 1);  p += sizeof(bodyPre) - 1;
  memcpy(p, entityId, idLen);               p += idLen;
  memcpy(p, bodyPost, sizeof(bodyPost));    // includes the NUL

  itemP->csr     = csr;
  itemP->url     = url;
  itemP->body    = body;
  itemP->bodyLen = (int) (p - body) + (int) sizeof(bodyPost) - 1;
  itemP->hasVerb = true;
  itemP->verb    = SwVerbPost;
}



// buildForwardUrl - compose retrieveEntity URL for one (csr, riP) pair
//
static char* buildForwardUrl(LdRegCacheItem* csr, LdRegInfo* riP, const char* entityId, bool forceTypeScope)
{
  const char* base = csr->endpoint;
  const char* path = "/ngsi-ld/v1/entities/";

  // URL composition, distop forwarding:
  //   * `?type=` is dropped for ALL modes — type-narrowing the forward is
  //     unsound for multi-typed entities (same id can be Vehicle on one
  //     broker and Habitation on another); the type filter is a LOCAL
  //     concern (applied client-side when building the response / entity
  //     map). The upstream knows only the entity id.
  //   * `&pick=` from the RegistrationInfo is included for ALL modes
  //     (including aux): the registering source advertised a limited
  //     attribute set, so we must not over-query — the upstream knows
  //     nothing about our registration of it.
  //   * `?sysAttrs=true` is required for the timestamp-based merge of
  //     inclusive/redirect, and conservatively kept for exclusive.
  //     Auxiliary doesn't need it internally (gap-fill is structural),
  //     so for aux we pass through the user's own `sysAttrs` flag —
  //     forwarded only if the original GET asked for it.
  bool isAux = (csr->mode == LdRegModeAuxiliary);

  const char* qs       = (isAux && !swNgsild.sysAttrs) ? "" : "?sysAttrs=true";
  const char* pickRaw  = buildInfoPickParam(csr, riP, &swRest.kalloc, forceTypeScope);
  const char* pick     = pickRaw;
  if (qs[0] == '\0' && pickRaw[0] == '&')
  {
    // First param: convert leading "&pick=" to "?pick=" in the mutable
    // buildInfoPickParam buffer. (The empty `""` case returns a literal
    // and is excluded by `pickRaw[0] == '&'`.)
    char* pb = (char*) pickRaw;
    pb[0] = '?';
    pick = pb;
  }

  int baseLen = strlen(base);
  int pathLen = strlen(path);
  int idLen   = strlen(entityId);
  int qsLen   = strlen(qs);
  int pickLen = strlen(pick);
  char* url   = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + idLen + qsLen + pickLen + 1);

  strcpy(url, base);
  strcpy(url + baseLen, path);
  strcpy(url + baseLen + pathLen, entityId);
  strcpy(url + baseLen + pathLen + idLen, qs);
  strcpy(url + baseLen + pathLen + idLen + qsLen, pick);
  return url;
}



// shapeUpstreamBody - shape a 2xx upstream body (parsed at reception) into our storage form
//
// Takes the response tree already parsed by the distop layer (ldDistOpResultTree).
// Returns NULL on an empty/malformed body (errorDetail set), otherwise the shaped
// tree ready for merging. Caller has already ruled out non-2xx codes.
//
static KjNode* shapeUpstreamBody(KjNode* treeP, const char* respCtxUrl, const char** errorDetailPP)
{
  if (treeP == NULL)
  {
    *errorDetailPP = "empty or malformed body in upstream 2xx response";
    return NULL;
  }

  // Query-form forwards (queryEntity / queryBatch — § 9.2 ops-aware
  // conversion of a retrieve) answer with an entity ARRAY; the retrieve
  // targeted one id, so unwrap the single element ([] → no entity).
  if (treeP->type == KjArray)
  {
    treeP = treeP->value.firstChildP;
    if (treeP == NULL)
    {
      *errorDetailPP = "upstream query-form response carried no entity";
      return NULL;
    }
    treeP->next = NULL;
  }

  // Expand via the context that travels WITH the response — the URL in its
  // json-ld#context Link header (application/json), else core. swldExpandTree
  // additionally applies any embedded @context (application/ld+json) on top.
  // NOT swNgsild.contextP: the response speaks the CP's vocabulary, which may
  // differ from the client's request context.
  SwldContext* respCtxP = (respCtxUrl != NULL) ? swldContextFromUrl(respCtxUrl, &swRest.kalloc) : NULL;
  if (respCtxP == NULL)
    respCtxP = swldCoreContext();

  swldExpandTree(treeP, respCtxP, &swRest.kalloc);
  ldStripAtContext(treeP);
  apiAttrToStorageWrap(treeP, swRest.kjsonP);
  ldExpiresAtPropagate(treeP, swRest.kjsonP);
  return treeP;
}



// ensureEntityId - inject id into the upstream tree if pick stripped it.
//
// For retrieveEntity the broker knows the id (it's literally the URL it
// forwarded to) so there is no value in adding `id` to the forward `pick=`
// — re-add it here after the response comes back. `type` is NOT
// reconstructible (entities may carry multiple types and the CSR only
// pins the one used for matching), so it must come back from upstream:
// see buildInfoPickParam, which prepends `type` to the forward pick list
// for the same reason.
//
static void ensureEntityId(KjNode* treeP, const char* entityId)
{
  if (treeP == NULL || treeP->type != KjObject) return;
  if (kjLookup(treeP, "id") != NULL)            return;
  kjChildAdd(treeP, kjString(swRest.kjsonP, "id", (char*) entityId));
}



// -----------------------------------------------------------------------------
//
// distributedRetrieveOne - § 5.7.1.4 single-entity distributed assemble
//
// Reads the local copy and merges every type-matched registration's
// contribution per § 4.5.5.3: exclusive/redirect strip the locally-held
// registered attrs then forward; inclusive merges (newest wins);
// auxiliary fills gaps only. Returns the merged storage-shape entity (in
// the request arena), or NULL when no source has it.
//
// `typeV` (NULL-terminated expanded type IRIs, or NULL) scopes the
// exclusive/redirect/inclusive reg match — only registrations serving the
// type are forwarded to, instead of every registration covering the id.
// Auxiliary is matched by id regardless of type (it only ever fills gaps).
//
// `*matchedP` is set true when at least one registration matched: the
// caller uses it to tell "no distributed handling, fall through to a plain
// local read" (false) from "distributed result is authoritative" (true).
//
// `wholeForward` forces type+scope into each forward pick so the merged
// entity is complete even with no request projection to consult — used by
// the linked-entity assemble, which always wants the whole target.
//
// On an exclusive-source failure the merge aborts: returns NULL with
// *errP populated (status 502). The caller decides whether that is fatal
// (retrieveEntity → 502) or merely a missed link (join → leave unfollowed).
// DistRetrieveErr + this prototype live in getEntity.h.
//
KjNode* distributedRetrieveOne(const char* entityId, char** typeV, Tenant* tP,
                               bool wholeForward, bool* matchedP, DistRetrieveErr* errP)
{
  if (matchedP != NULL) *matchedP = false;
  if (errP != NULL) { errP->status = 0; errP->noEndpoint = false; errP->upstreamDetail = NULL; errP->regId = NULL; errP->upstreamCode = 0; }
  if (entityId == NULL || tP == NULL)
    return NULL;

  LdRegCacheItem**  exclV  = NULL;
  LdRegCacheItem**  redirV = NULL;
  LdRegCacheItem**  inclV  = NULL;
  LdRegCacheItem**  auxV   = NULL;
  int               exclN  = 0;
  int               redirN = 0;
  int               inclN  = 0;
  int               auxN   = 0;

  if (tP->regCacheP != NULL)
  {
    exclN  = ldRegCacheMatchForRetrieve((LdRegCache*) tP->regCacheP, entityId, typeV, LdRegModeExclusive, &exclV);
    redirN = ldRegCacheMatchForRetrieve((LdRegCache*) tP->regCacheP, entityId, typeV, LdRegModeRedirect,  &redirV);
    inclN  = ldRegCacheMatchForRetrieve((LdRegCache*) tP->regCacheP, entityId, typeV, LdRegModeInclusive, &inclV);
    auxN   = ldRegCacheMatchForRetrieve((LdRegCache*) tP->regCacheP, entityId, NULL,  LdRegModeAuxiliary, &auxV);
  }

  // Loop detection per § 5.7.5 — our own Via alias already inbound → serve
  // locally, suppress forwards.
  const char* ownAlias = ldCsourceAliasForTenant(tP->name, &swRest.kalloc);
  if (ldDistOpLoopDetected(ownAlias))
  {
    ldRegCacheMatchRelease(exclV,  exclN);  exclV  = NULL; exclN  = 0;
    ldRegCacheMatchRelease(redirV, redirN); redirV = NULL; redirN = 0;
    ldRegCacheMatchRelease(inclV,  inclN);  inclV  = NULL; inclN  = 0;
    ldRegCacheMatchRelease(auxV,   auxN);   auxV   = NULL; auxN   = 0;
  }

  if (exclN == 0 && redirN == 0 && inclN == 0 && auxN == 0)
    return NULL;   // no distributed handling — *matchedP stays false

  if (matchedP != NULL) *matchedP = true;

  // Local lookup (always — per § 5.7.1.4)
  KjNode* destP = NULL;
  db.entityRetrieve(tP, entityId, &destP);

  int64_t nowNs = nowNanoseconds();

  LdRegCacheItem** groups[]  = { exclV, redirV, inclV, auxV };
  int              counts[]  = { exclN, redirN, inclN, auxN };
  bool             stripG[]  = { true,  true,   false, false };

  // Exclusive endpoint check: fail-fast 502 BEFORE we batch.
  for (int i = 0; i < exclN; i++)
  {
    if (exclV[i]->endpoint == NULL)
    {
      if (errP != NULL) { errP->status = 502; errP->noEndpoint = true; errP->regId = exclV[i]->regId; }
      ldRegCacheMatchRelease(exclV,  exclN);
      ldRegCacheMatchRelease(redirV, redirN);
      ldRegCacheMatchRelease(inclV,  inclN);
      ldRegCacheMatchRelease(auxV,   auxN);
      return NULL;
    }
  }

  int total = 0;
  for (int g = 0; g < 4; g++)
    for (int i = 0; i < counts[g]; i++)
      for (LdRegInfo* riP = groups[g][i]->infoV; riP != NULL; riP = riP->next) total++;

  LdDistOpBatchItem*   items     = (LdDistOpBatchItem*)   kaAlloc(&swRest.kalloc, total * sizeof(LdDistOpBatchItem));
  memset(items, 0, total * sizeof(LdDistOpBatchItem));
  LdDistOpBatchResult* results   = (LdDistOpBatchResult*) kaAlloc(&swRest.kalloc, total * sizeof(LdDistOpBatchResult));
  int*                 itemGroup = (int*)                 kaAlloc(&swRest.kalloc, total * sizeof(int));
  LdRegInfo**          itemRiP   = (LdRegInfo**)          kaAlloc(&swRest.kalloc, total * sizeof(LdRegInfo*));
  int                  itemCount = 0;
  memset(results, 0, total * sizeof(LdDistOpBatchResult));

  for (int g = 0; g < 4; g++)
  {
    for (int i = 0; i < counts[g]; i++)
    {
      LdRegCacheItem* csr = groups[g][i];
      if (csr->endpoint == NULL) continue;
      if (ldDistOpCsrWouldLoop(csr, ownAlias)) continue;

      bool canRetrieve = ldRegOpSupported(csr, LdOpRetrieveEntity);
      bool canQE       = ldRegOpSupported(csr, LdOpQueryEntities);
      bool canQB       = ldRegOpSupported(csr, LdOpBatchQuery);
      if (!canRetrieve && !canQE && !canQB) continue;

      for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
      {
        if (!infoEntryMatchesEntity(riP, entityId)) continue;

        if (canRetrieve)
        {
          items[itemCount].csr     = csr;
          items[itemCount].url     = buildForwardUrl(csr, riP, entityId, wholeForward);
          items[itemCount].body    = NULL;
          items[itemCount].bodyLen = 0;
        }
        else if (canQE)
        {
          items[itemCount].csr     = csr;
          items[itemCount].url     = buildQueryFormUrl(csr, riP, entityId, wholeForward);
          items[itemCount].body    = NULL;
          items[itemCount].bodyLen = 0;
        }
        else
          buildQueryBatchForm(csr, riP, entityId, &items[itemCount]);

        itemGroup[itemCount]     = g;
        itemRiP[itemCount]       = riP;
        itemCount++;
      }
    }
  }

  if (itemCount > 0)
  {
    ldDistOpSendMulti(items, itemCount, SwVerbGet, ownAlias, results);

    // Pass 1 — § 4.3.6.3 local-strip for excl/redir BEFORE any merge.
    for (int i = 0; i < itemCount; i++)
    {
      if (stripG[itemGroup[i]] && destP != NULL)
        stripInfoAttrsFromLocal(destP, itemRiP[i]);
    }

    // Pass 2 — merge the upstream results.
    for (int i = 0; i < itemCount; i++)
    {
      int g      = itemGroup[i];
      int upCode = results[i].statusCode;

      if (upCode == 404 && g == 0) continue;  // exclusive: 404 tolerated
      if (upCode < 200 || upCode >= 300)
      {
        if (g == 0)
        {
          // Exclusive non-2xx (except 404) → abort with a 502 for the caller.
          if (errP != NULL)
          {
            errP->status         = 502;
            errP->upstreamDetail = results[i].errorDetail;
            errP->regId          = (items[i].csr != NULL) ? items[i].csr->regId : NULL;
            errP->upstreamCode   = upCode;
          }
          ldRegCacheMatchRelease(exclV,  exclN);
          ldRegCacheMatchRelease(redirV, redirN);
          ldRegCacheMatchRelease(inclV,  inclN);
          ldRegCacheMatchRelease(auxV,   auxN);
          return NULL;
        }
        continue;  // redir/incl/aux: tolerate failures
      }

      KjNode* upP = results[i].responseTree;
      if (upP == NULL) continue;

      if (upP->type == KjArray)
      {
        upP = upP->value.firstChildP;
        if (upP == NULL) continue;
        upP->next = NULL;
      }

      SwldContext* respCtxP = (results[i].responseContextUrl != NULL)
                              ? swldContextFromUrl(results[i].responseContextUrl, &swRest.kalloc) : NULL;
      if (respCtxP == NULL)
        respCtxP = swldCoreContext();
      swldExpandTree(upP, respCtxP, &swRest.kalloc);
      ldStripAtContext(upP);
      apiAttrToStorageWrap(upP, swRest.kjsonP);
      ldExpiresAtPropagate(upP, swRest.kjsonP);
      ensureEntityId(upP, entityId);

      if (destP == NULL)
        destP = upP;
      else if (g == 3)
        mergeAuxiliaryInto(destP, upP);
      else
        mergeOneSourceInto(destP, upP, nowNs);
    }
  }

  ldRegCacheMatchRelease(exclV,  exclN);
  ldRegCacheMatchRelease(redirV, redirN);
  ldRegCacheMatchRelease(inclV,  inclN);
  ldRegCacheMatchRelease(auxV,   auxN);

  return destP;
}



// -----------------------------------------------------------------------------
//
// getEntity -
//
// -----------------------------------------------------------------------------
//
// geoJsonGeomProtectSetup - see getEntities.c. For a geo+json response, mark
// the selected geometry GeoProperty (expanded IRI) so the pick/omit/attrs
// projection keeps it (ldToGeoJson needs it for "geometry", § 5.3.3.2);
// geoJsonGeomForced records whether the user's projection would have dropped
// it, so ldToGeoJson prunes it from "properties".
//
static void geoJsonGeomProtectSetup(void)
{
  if (ldAcceptParse(swRest.in.accept) != LdAcceptGeoJson)
    return;

  SwldContext* ctxP   = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();
  const char*  gmName = (swNgsild.geometryProperty != NULL) ? swNgsild.geometryProperty : "location";
  char*        gmIri  = swldExpand(ctxP, gmName, &swRest.kalloc, NULL, NULL);
  if (gmIri == NULL)
    gmIri = (char*) gmName;

  swNgsild.geometryPropertyExpanded = gmIri;

  bool wanted = true;
  if (swNgsild.pickV != NULL)
  {
    wanted = false;
    for (int i = 0; swNgsild.pickV[i] != NULL; i++)
      if (strcmp(swNgsild.pickV[i], gmIri) == 0) { wanted = true; break; }
  }
  else if (swNgsild.omitV != NULL)
  {
    for (int i = 0; swNgsild.omitV[i] != NULL; i++)
      if (strcmp(swNgsild.omitV[i], gmIri) == 0) { wanted = false; break; }
  }
  else if (swNgsild.attrsV != NULL)
  {
    wanted = false;
    for (int i = 0; swNgsild.attrsV[i] != NULL; i++)
      if (strcmp(swNgsild.attrsV[i], gmIri) == 0) { wanted = true; break; }
  }

  swNgsild.geoJsonGeomForced = !wanted;
}



bool getEntity(void)
{
  // § 4.21 / § 6.4.3 — cross-parameter projection validation
  // (pick ∩ omit, pick + attrs, omit + attrs, etc).
  if (ldParamsValidate())
    return true;

  // geo+json: protect the geometry GeoProperty through the member projection.
  geoJsonGeomProtectSetup();

  const char* entityId = swRest.in.wildcard[0];

  //
  // § 6.3.22 / § 5.5.15 — snapshot-aware read. NGSILD-Snapshot header
  // forces local-only reads from the named snapshot's frozen store.
  // Distop dispatch is bypassed entirely.
  //
  {
    bool seen = false;
    LdSnapshotCacheItem* snapItem = ldSnapshotItemFromHeader(&seen);
    if (seen)
    {
      if (snapItem == NULL) return true;            // 404 raised by helper
      return snapshotGetEntity(snapItem, entityId);
    }
  }

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
    Tenant*          tP      = (Tenant*) swNgsild.tenantP;
    bool             matched = false;
    DistRetrieveErr  err     = {0};

    // Request-driven retrieve: scope the reg match by the request's type
    // (NULL = no type filter), and let buildInfoPickParam honour the
    // request projection for the forward pick (wholeForward = false).
    KjNode*          destP   = distributedRetrieveOne(entityId, swNgsild.typeV, tP, false, &matched, &err);

    if (matched)
    {
      if (err.noEndpoint)
      {
        ldError(502, LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                "exclusive registration '%s' has no endpoint", err.regId);
        return true;
      }
      if (err.status != 0)
      {
        // Exclusive source failed — the registrationId / upstream status
        // ride along as structured ProblemDetails members (RFC 9457 §3.2).
        if (err.upstreamDetail)
          ldError(502, LD_ERROR_INTERNAL_ERROR, "Bad Gateway", "forwarded request failed: %s", err.upstreamDetail);
        else
          ldError(502, LD_ERROR_INTERNAL_ERROR, "Bad Gateway", "forwarded request failed (status %d)", err.upstreamCode);
        ldErrorExtraString("registrationId", err.regId);
        if (err.upstreamCode > 0)
          ldErrorExtraInt("statusCode", err.upstreamCode);
        return true;
      }

      if (destP == NULL)
      {
        ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                "entity '%s' not found", entityId);
        return true;
      }

      if (swNgsild.pickV != NULL || swNgsild.omitV != NULL)
        ldPickOmit(destP, swNgsild.pickV, swNgsild.omitV);
      else if (swNgsild.attrsV != NULL)
        ldAttrsFilter(destP, swNgsild.attrsV);
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

  // Apply pick/omit attribute projection (or the legacy attrs alias).
  // Each path then enforces its own 404-on-empty rule:
  //
  //   pick / omit (§ 6.3.6): "If the resulting Entity contains no
  //     members, the operation shall return ResourceNotFound." Members
  //     are id / type / optional scope / user attributes. @context is a
  //     JSON-LD expansion aux (not a member). createdAt / modifiedAt /
  //     expiresAt are SystemAttributes — included only when
  //     ?sysAttrs=true, so without that flag they'd be stripped before
  //     render and don't count.
  //
  //   attrs (§ 6.4.3.2 / § 5.10.2): the deprecated alias is "show only
  //     the listed attributes AND only on entities that have at least
  //     one of them". For single-entity retrieve, none-matching → 404.
  //     Entity members (id / type / scope) are preserved by the filter
  //     but don't count toward the "at least one matching attribute"
  //     test — only user attributes do.
  //
  // ETSI 018_03_02 / 018_19_*.
  if (swNgsild.pickV != NULL || swNgsild.omitV != NULL)
  {
    ldPickOmit(entityP, swNgsild.pickV, swNgsild.omitV);

    bool hasMembers = false;
    for (KjNode* c = entityP->value.firstChildP; c != NULL; c = c->next)
    {
      if (c->name == NULL)                          continue;
      if (strcmp(c->name, "@context")        == 0)  continue;
      if (!swNgsild.sysAttrs &&
          (strcmp(c->name, LD_VOCAB_CREATED_AT)  == 0 ||
           strcmp(c->name, LD_VOCAB_MODIFIED_AT) == 0 ||
           strcmp(c->name, LD_VOCAB_EXPIRES_AT)  == 0))
        continue;
      hasMembers = true;
      break;
    }
    if (!hasMembers)
    {
      // § 5.7.1 + § 4.21: spec text doesn't explicitly say what to
      // return when pick/omit reduce an entity to no members. ETSI's
      // official test 018_19_01/02 (since v1.8.1) expects
      // ResourceNotFound — the test reads "no projectable members"
      // as equivalent to "endpoint doesn't know about this entity
      // (with this projection)". We defer to that interpretation.
      ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
              "entity '%s' has no members after pick/omit projection", entityId);
      return true;
    }
  }
  else if (swNgsild.attrsV != NULL)
  {
    ldAttrsFilter(entityP, swNgsild.attrsV);

    // Count surviving user attributes (skip entity members and
    // system-managed timestamps — the filter preserves them but they
    // don't satisfy the attrs existence rule).
    bool hasUserAttr = false;
    for (KjNode* c = entityP->value.firstChildP; c != NULL; c = c->next)
    {
      if (c->name == NULL)                            continue;
      if (strcmp(c->name, "id")               == 0)   continue;
      if (strcmp(c->name, "type")             == 0)   continue;
      if (strcmp(c->name, "@context")         == 0)   continue;
      if (strcmp(c->name, LD_VOCAB_SCOPE)     == 0)   continue;
      if (strcmp(c->name, LD_VOCAB_CREATED_AT)  == 0) continue;
      if (strcmp(c->name, LD_VOCAB_MODIFIED_AT) == 0) continue;
      if (strcmp(c->name, LD_VOCAB_EXPIRES_AT)  == 0) continue;
      hasUserAttr = true;
      break;
    }
    if (!hasUserAttr)
    {
      ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
              "entity '%s' has none of the requested attributes", entityId);
      return true;
    }
  }

  // § 4.5.23 — linked-entity expansion. join=flat returns an array
  // (primary + targets); join=inline nests targets as `entity` sub-
  // attributes on the originating Relationship. @none and absent
  // leave the response as the single entity.
  if (swNgsild.join != NULL)
  {
    int level = (swNgsild.joinLevel > 0) ? swNgsild.joinLevel : 1;
    if (strcmp(swNgsild.join, "flat") == 0)
    {
      KjNode* flatP = ldLinkedEntitiesFlat(entityP, level, (Tenant*) swNgsild.tenantP);
      // Single-entity Retrieve shape: if flat returned only the
      // primary (no linked targets resolved), unwrap the array so the
      // response stays a plain Entity object. § 4.5.23 doesn't mandate
      // a single-element array — the array shape is for "primary +
      // targets" and degenerates to "just the primary" when no targets
      // were found.
      if (flatP != NULL && flatP->type == KjArray)
      {
        KjNode* first = flatP->value.firstChildP;
        if (first != NULL && first->next == NULL)
        {
          first->name = NULL;
          flatP = first;
        }
      }
      swRest.out.responseTree = flatP;
      return true;
    }
    if (strcmp(swNgsild.join, "inline") == 0)
    {
      swRest.out.responseTree = ldLinkedEntitiesInline(entityP, level, (Tenant*) swNgsild.tenantP);
      return true;
    }
  }

  swRest.out.responseTree = entityP;
  return true;
}

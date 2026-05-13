//
// FILE            getEntities.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stddef.h>                                  // NULL
#include <stdio.h>                                   // snprintf
#include <string.h>                                  // strcmp, strlen, strcpy

#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjParse.h"                           // kjParse
#include "kjson/kjBuilder.h"                         // kjArray, kjObject, kjChildAdd, kjChildRemove
#include "kjson/kjChildReplace.h"                    // kjChildReplace
#include "swRest/SwRestState.h"                      // swRest
#include "swRest/swRestClient.h"                     // SwRestClientRequest, swRestClientSend
#include "swRest/swRestOutHeader.h"                  // swRestOutHeaderAdd
#include "swJsonld/swldExpand.h"                     // swldExpand
#include "swJsonld/swldExpandTree.h"                 // swldExpandTree
#include "swJsonld/swldCompact.h"                    // swldCompact
#include "swJsonld/swldInit.h"                       // swldCoreContext
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldParamsValidate.h"               // ldParamsValidate
#include "swNgsild/ldOrderSort.h"                    // ldOrderSort
#include "swNgsild/ldStripAtContext.h"              // ldStripAtContext
#include "swNgsild/ldExpiresAtPropagate.h"          // ldExpiresAtPropagate
#include "swNgsild/ldAcceptParse.h"                 // ldAcceptParse, LdAcceptType
#include "swNgsild/ldEntityMatch.h"                  // ldEntityMatchType, ldEntityMatchQ, ldEntityMatchScope
#include "swNgsild/ldDistMerge.h"                    // ldDistInstanceShouldReplace, ldDistInstanceIsExpired
#include "swNgsild/ldQAttrs.h"                       // ldQAttrs
#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieve
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "swNgsild/ldDistOp.h"                       // ldDistOpCsrWouldLoop
#include "swNgsild/LdEntityMap.h"                    // LdEntityMap, LdEntityMapStore
#include "swNgsild/ldEntityMap.h"                    // ldEntityMapCreate, ldEntityMapAddEntry, ldEntityMapToTree

#include "db/DbDriver.h"                             // db, DB_OK
#include "db/Tenant.h"                               // Tenant

#include "linkedEntities/ldLinkedEntities.h"         // ldLinkedEntitiesExpandArrayFlat / Inline

#include "serviceRoutines/ldSnapshotRead.h"          // ldSnapshotItemFromHeader, snapshotGetEntities
#include "serviceRoutines/getEntities.h"             // Own interface



//
// linkedFetcher - LdQEntityFetchFunc wrapper around linkedFetchOne
//
// linkedFetchOne tries local DB first, then walks the reg-cache for any
// CSR covering the entity id and forwards a GET via dist-op. The result
// is in storage shape, ready for the q-evaluator's reads.
//
static int linkedFetcher(const char* entityId, KjNode** entityPP, void* userData)
{
  return linkedFetchOne(entityId, entityPP, (Tenant*) userData);
}



//
// qHasLinked - true if a q-expression tree contains any LdQLinkedNode
//
static bool qHasLinked(LdQNode* nodeP)
{
  if (nodeP == NULL)
    return false;
  if (nodeP->type == LdQLinkedNode)
    return true;
  if (nodeP->type == LdQAndNode || nodeP->type == LdQOrNode)
  {
    for (int i = 0; i < nodeP->group.count; i++)
      if (qHasLinked(nodeP->group.childV[i]))
        return true;
  }
  return false;
}



//
// applyLinkedQPostFilter - prune arrayP entries whose q-expression has
// a LinkedNode that doesn't match (BSON layer can't evaluate it).
//
static void applyLinkedQPostFilter(KjNode* arrayP)
{
  if (arrayP == NULL || arrayP->type != KjArray)
    return;
  if (swNgsild.qExpr == NULL || !qHasLinked(swNgsild.qExpr))
    return;

  Tenant* tP = (Tenant*) swNgsild.tenantP;

  KjNode* entityP = arrayP->value.firstChildP;
  while (entityP != NULL)
  {
    KjNode* nextP = entityP->next;

    if (!ldEntityMatchQEx(entityP, swNgsild.qExpr, linkedFetcher, tP))
      kjChildRemove(arrayP, entityP);

    entityP = nextP;
  }
}



// -----------------------------------------------------------------------------
//
// applyResultFilters - apply type/q/scopeQ/geoQ on assembled entities
//
// Used by:
//   * split-mode post-aggregation (§ 5.7.2.4 — filters were stripped before
//     forwarding, must re-evaluate on the merged entity)
//   * entityMap-paginated path (§ 5.7.2.4 — the map fixes which sources hold
//     each entity, but the current request's filters still apply per the
//     conditions list at lines 5586-5599 of the spec)
//
// q is evaluated with the linkedFetcher so § 4.9 sub-queries (q=rel{...})
// resolve their Relationship target correctly. Entities are in storage
// shape on entry (post-merge / post-apiAttrToStorageWrap).
//
static void applyResultFilters(KjNode* arrayP)
{
  if (arrayP == NULL || arrayP->type != KjArray)
    return;

  KjNode* entityP = arrayP->value.firstChildP;
  while (entityP != NULL)
  {
    KjNode* nextP = entityP->next;
    bool    keep  = true;

    if (keep && swNgsild.typeExpr != NULL)
    {
      KjNode* typeP = kjLookup(entityP, "type");
      if (!ldEntityMatchType(typeP, swNgsild.typeExpr))
        keep = false;
    }

    if (keep && swNgsild.qExpr != NULL)
    {
      if (!ldEntityMatchQEx(entityP, swNgsild.qExpr, linkedFetcher, (Tenant*) swNgsild.tenantP))
        keep = false;
    }

    if (keep && swNgsild.scopeExpr != NULL)
    {
      KjNode* scopeP = kjLookup(entityP, "scope");
      if (!ldEntityMatchScope(scopeP, swNgsild.scopeExpr))
        keep = false;
    }

    if (keep && swNgsild.geoRel != NULL && db.geoMatchFunc != NULL)
    {
      if (!db.geoMatchFunc(entityP, swNgsild.geoRel, swNgsild.geometry,
                           swNgsild.coordinates,
                           swNgsild.geoproperty ? swNgsild.geoproperty : "location"))
        keep = false;
    }

    if (!keep)
      kjChildRemove(arrayP, entityP);

    entityP = nextP;
  }
}



// -----------------------------------------------------------------------------
//
// computeWantedAttrs - the set of attrs the broker needs back from each CSR
//
// Returns NULL when the user has no `pick` (they want every attribute the
// CSR has). Returns a NULL-terminated array of expanded attr IRIs otherwise:
//
//   user.pick                      ∪
//   q-referenced attrs             ∪   (so a local re-eval of q after merge stays sound)
//   geo property (if hasGeoQ)      ∪   (so a local re-eval of geoQ stays sound)
//   geometry property (if Accept   )   (so the geo+json renderer has the geometry)
//      application/geo+json
//
// Allocated in the request arena. Strings are borrowed where possible
// (pickV and ldQAttrs results are already in the arena), expanded on
// demand for the geo property short-name fallback.
//
static char** computeWantedAttrs(KAlloc* kaP)
{
  if (swNgsild.pickV == NULL)
    return NULL;

  // Worst-case capacity: pickV count + q-attr count + 2 (geo + geometry)
  int pickN = 0;
  while (swNgsild.pickV[pickN] != NULL)
    pickN++;

  char** qV   = ldQAttrs(swNgsild.qExpr, kaP);
  int    qN   = 0;
  if (qV != NULL)
    while (qV[qN] != NULL)
      qN++;

  bool hasGeoQ        = (swNgsild.georel != NULL);
  bool acceptGeoJson  = (ldAcceptParse(swRest.in.accept) == LdAcceptGeoJson);

  int cap = pickN + qN + 2;
  char** wanted = (char**) kaAlloc(kaP, (cap + 1) * sizeof(char*));
  int    n      = 0;

  // Helper: append if not already present (linear dedupe; n always small)
  #define WANT_ADD(x) do {                                                   \
    const char* _x = (x);                                                     \
    if (_x == NULL) break;                                                    \
    bool _dup = false;                                                        \
    for (int _i = 0; _i < n; _i++) if (strcmp(wanted[_i], _x) == 0) { _dup = true; break; } \
    if (!_dup) wanted[n++] = (char*) _x;                                      \
  } while (0)

  for (int i = 0; i < pickN; i++)
    WANT_ADD(swNgsild.pickV[i]);

  for (int i = 0; i < qN; i++)
    WANT_ADD(qV[i]);

  if (hasGeoQ)
  {
    const char* gp = swNgsild.geoproperty
                     ? swNgsild.geoproperty
                     : swldExpand(swNgsild.contextP, "location", kaP, NULL, NULL);
    WANT_ADD(gp);
  }

  if (acceptGeoJson && swNgsild.geometryProperty != NULL)
  {
    char* gmp = swldExpand(swNgsild.contextP, swNgsild.geometryProperty, kaP, NULL, NULL);
    WANT_ADD(gmp);
  }

  #undef WANT_ADD

  wanted[n] = NULL;
  return wanted;
}



// -----------------------------------------------------------------------------
//
// buildPickParam - render a pick=A,B,C URL fragment from an IRI vector
//
// vec is NULL-terminated, IRIs preferred (compactable via core context get
// the short form, others stay as IRI). Returns "" if vec is NULL or empty.
// Allocates in kaP.
//
// Always appends id,type,scope so the upstream CSR returns those Entity
// members alongside the requested Attributes — § 6.3.6 pick strips
// non-listed members, including id+type, which would leave us unable to
// aggregate the response by entity id.
//
static const char* buildPickParam(char** vec, KAlloc* kaP)
{
  if (vec == NULL || vec[0] == NULL)
    return "";

  int totalLen = 0;
  for (int i = 0; vec[i] != NULL; i++)
  {
    const char* c = swldCompact(swldCoreContext(), vec[i]);
    totalLen += strlen(c ? c : vec[i]) + 1;
  }
  totalLen += sizeof("id,type,scope,");  // upper bound on the appended suffix

  char* buf = (char*) kaAlloc(kaP, 6 + totalLen + 1);
  strcpy(buf, "&pick=");
  int pos = 6;
  for (int i = 0; vec[i] != NULL; i++)
  {
    if (pos > 6) buf[pos++] = ',';
    const char* c = swldCompact(swldCoreContext(), vec[i]);
    const char* n = c ? c : vec[i];
    strcpy(buf + pos, n);
    pos += strlen(n);
  }
  if (pos > 6) buf[pos++] = ',';
  memcpy(buf + pos, "id,type,scope", sizeof("id,type,scope"));  // includes NUL
  pos += sizeof("id,type,scope") - 1;
  return buf;
}



// -----------------------------------------------------------------------------
//
// intersectAndPick - intersect `wanted` with reg's exports, render pick param
//
// propV/relV : NULL-terminated arrays of expanded IRIs that the source
//              "exports". Either may be NULL. Both NULL means the source
//              exports everything (no restriction).
// wanted     : NULL = user wants every attr the source has, char** = the
//              IRI set the broker needs back
// kaP        : output allocator
// outSkipP   : *outSkipP set to true if user.pick was set AND the
//              intersection is empty (caller must skip this CSR)
//
// Returns the rendered "&pick=A,B,C" fragment, "" when no pick should
// be sent (source exports everything AND user wants everything).
//
static const char* intersectAndPick(char** propV, char** relV, char** wanted, KAlloc* kaP, bool* outSkipP)
{
  *outSkipP = false;

  bool regRestricts = (propV != NULL || relV != NULL);

  if (wanted == NULL)
  {
    // User wants everything.
    if (!regRestricts)
      return "";  // exports everything → no pick

    // Reg restricts → forward exports as pick (avoids overquery on attrs the reg won't deliver)
    int    cap = 0;
    char** lists[] = { propV, relV, NULL };
    for (int li = 0; lists[li] != NULL; li++)
      for (int a = 0; lists[li][a] != NULL; a++)
        cap++;

    char** vec = (char**) kaAlloc(kaP, (cap + 1) * sizeof(char*));
    int    n   = 0;
    for (int li = 0; lists[li] != NULL; li++)
      for (int a = 0; lists[li][a] != NULL; a++)
        vec[n++] = lists[li][a];
    vec[n] = NULL;
    return buildPickParam(vec, kaP);
  }

  // User has pick: intersect(wanted, exports).
  if (!regRestricts)
    return buildPickParam(wanted, kaP);  // exports all → forward wanted as-is

  int wantedN = 0;
  while (wanted[wantedN] != NULL)
    wantedN++;

  char** narrowed = (char**) kaAlloc(kaP, (wantedN + 1) * sizeof(char*));
  int    nN       = 0;

  for (int w = 0; w < wantedN; w++)
  {
    bool found = false;

    char** lists[] = { propV, relV, NULL };
    for (int li = 0; lists[li] != NULL && !found; li++)
      for (int a = 0; lists[li][a] != NULL; a++)
      {
        if (strcmp(wanted[w], lists[li][a]) == 0) { found = true; break; }
      }

    if (found)
      narrowed[nN++] = wanted[w];
  }
  narrowed[nN] = NULL;

  if (nN == 0)
  {
    // Nothing the user wants is exported by this source — skip.
    *outSkipP = true;
    return "";
  }

  return buildPickParam(narrowed, kaP);
}



// -----------------------------------------------------------------------------
//
// csrUnionExports - union of all infoV[*].propertyNamesV + relationshipNamesV
//
// Split-mode forwards are CSR-level (not per-RegistrationInfo), so the
// "exports" set the broker should narrow against is the union across
// every info entry of the CSR. Returns a NULL-terminated, deduped char**
// allocated in kaP, or NULL when the CSR has no restriction at all
// (every info entry has propertyNamesV == relationshipNamesV == NULL).
//
static char** csrUnionExports(LdRegCacheItem* csr, KAlloc* kaP)
{
  // Worst-case capacity
  int cap = 0;
  bool anyRestricts = false;
  for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
  {
    if (riP->propertyNamesV != NULL || riP->relationshipNamesV != NULL)
      anyRestricts = true;
    char** lists[] = { riP->propertyNamesV, riP->relationshipNamesV, NULL };
    for (int li = 0; lists[li] != NULL; li++)
      for (int a = 0; lists[li][a] != NULL; a++)
        cap++;
  }

  if (!anyRestricts)
    return NULL;

  char** out = (char**) kaAlloc(kaP, (cap + 1) * sizeof(char*));
  int    n   = 0;
  for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
  {
    char** lists[] = { riP->propertyNamesV, riP->relationshipNamesV, NULL };
    for (int li = 0; lists[li] != NULL; li++)
      for (int a = 0; lists[li][a] != NULL; a++)
      {
        bool dup = false;
        for (int i = 0; i < n; i++)
          if (strcmp(out[i], lists[li][a]) == 0) { dup = true; break; }
        if (!dup) out[n++] = lists[li][a];
      }
  }
  out[n] = NULL;
  return out;
}



// -----------------------------------------------------------------------------
//
// apiAttrToStorageWrap - wrap upstream API-format entity into storage format
//
// Wraps each attribute: "speed": {type,value} → "speed": {"@none": {type,value}}
//
static void apiAttrToStorageWrap(KjNode* entityP)
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

    KjNode* wrapperP = kjObject(NULL, curP->name);
    kjChildReplace(entityP, curP, wrapperP);
    curP->name = (char*) "@none";
    curP->next = NULL;
    kjChildAdd(wrapperP, curP);

    curP = nextP;
  }
}



// -----------------------------------------------------------------------------
//
// srcMapAdd - record that `source` contributes to `entityId`
//
// The srcMap is a KjObject keyed by entityId whose value is a KjArray of
// source strings ("@none" for local, CSR's regId otherwise). Duplicates
// are suppressed — the same source contributing the same entity twice is
// collapsed into one list entry.
//
// Pass srcMap=NULL to skip (tracking is only needed when an EntityMap is
// being built, so the common query path pays zero cost).
//
static void srcMapAdd(KjNode* srcMap, const char* entityId, const char* source)
{
  if (srcMap == NULL || entityId == NULL || source == NULL)
    return;

  KjNode* arrP = kjLookup(srcMap, entityId);
  if (arrP == NULL)
  {
    arrP = kjArray(swRest.kjsonP, entityId);
    kjChildAdd(srcMap, arrP);
  }

  for (KjNode* s = arrP->value.firstChildP; s != NULL; s = s->next)
  {
    if (s->type == KjString && strcmp(s->value.s, source) == 0)
      return;
  }

  kjChildAdd(arrP, kjString(swRest.kjsonP, NULL, source));
}



// -----------------------------------------------------------------------------
//
// srcMapStampLocalFrom - add "@none" for every entity currently in arrayP
//
// Called after the local DB query (initial or split-mode re-query) so the
// srcMap reflects the current set of locally-held entity IDs.
//
static void srcMapStampLocalFrom(KjNode* srcMap, KjNode* arrayP)
{
  if (srcMap == NULL || arrayP == NULL || arrayP->type != KjArray)
    return;

  for (KjNode* ep = arrayP->value.firstChildP; ep != NULL; ep = ep->next)
  {
    KjNode* idP = kjLookup(ep, "id");
    if (idP != NULL && idP->type == KjString)
      srcMapAdd(srcMap, idP->value.s, "@none");
  }
}



// -----------------------------------------------------------------------------
//
// forwardQueryToCSR - forward GET /entities to a CSR, return the parsed response array
//
// Builds the URL from the CSR's endpoint + the original query string
// (for no-split mode, the full query is forwarded). Returns NULL on failure.
//
static KjNode* forwardQueryToCSR(LdRegCacheItem* csr, const char* queryString)
{
  if (csr->endpoint == NULL)
    return NULL;

  const char* base = csr->endpoint;
  const char* path = "/ngsi-ld/v1/entities?";
  int baseLen = strlen(base);
  int pathLen = strlen(path);
  int qsLen   = strlen(queryString);
  char* url   = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + qsLen + 1);

  strcpy(url, base);
  strcpy(url + baseLen, path);
  strcpy(url + baseLen + pathLen, queryString);

  SwRestClientRequest  req;
  SwRestClientResponse resp;

  swRestClientRequestInit(&req, SwVerbGet, url, &swRest.kalloc);
  swRestClientRequestTimeout(&req, 5000, 10000);

  int rc = swRestClientSend(&req, &resp);
  if (rc != 0 || resp.statusCode < 200 || resp.statusCode >= 300)
    return NULL;
  if (resp.body == NULL || resp.bodyLen == 0)
    return NULL;

  // Parse response body (need mutable copy for kjParse)
  char* bodyCopy = (char*) kaAlloc(&swRest.kalloc, resp.bodyLen + 1);
  memcpy(bodyCopy, resp.body, resp.bodyLen);
  bodyCopy[resp.bodyLen] = 0;

  KjNode* treeP = kjParse(swRest.kjsonP, bodyCopy);
  if (treeP != NULL)
    ldStripAtContext(treeP);
  return treeP;  // KjArray of entities (in API format)
}



// -----------------------------------------------------------------------------
//
// forwardEntityMapToCSR - GET /entityMaps to a CSR for distributed EntityMap build (§ 5.14.4.4).
//
// Builds GET <endpoint>/ngsi-ld/v1/entityMaps?<filters> and returns:
//   - KjArray of synthetic { "id": <entityId> } objects, one per key of the
//     remote EntityMap's "entityMap" object — feeds the existing dedup/merge
//     loop without changes
//   - *remoteMapIdOut → the remote EntityMap's id (if present), so the
//     local broker can record (csr.regId → remote-map-id) in linkedMaps
//
// Note: the spec binds Create-EntityMap-Query-Entity to GET (see
// spec-doubts #14 and createEntityMap.c) — counterintuitive but it returns
// 201 Created. The local POST /entityMaps route is a convenience alias
// that accepts no URL params.
//
// Returns NULL on failure.
//
static KjNode* forwardEntityMapToCSR(LdRegCacheItem* csr, const char* queryString, char** remoteMapIdOut)
{
  if (remoteMapIdOut != NULL)
    *remoteMapIdOut = NULL;

  if (csr->endpoint == NULL)
    return NULL;

  const char* base = csr->endpoint;
  const char* path = "/ngsi-ld/v1/entityMaps?";
  int baseLen = strlen(base);
  int pathLen = strlen(path);
  int qsLen   = strlen(queryString);
  char* url   = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + qsLen + 1);

  strcpy(url, base);
  strcpy(url + baseLen, path);
  strcpy(url + baseLen + pathLen, queryString);

  SwRestClientRequest  req;
  SwRestClientResponse resp;

  swRestClientRequestInit(&req, SwVerbGet, url, &swRest.kalloc);
  swRestClientRequestTimeout(&req, 5000, 10000);

  int rc = swRestClientSend(&req, &resp);
  if (rc != 0 || resp.statusCode < 200 || resp.statusCode >= 300)
    return NULL;
  if (resp.body == NULL || resp.bodyLen == 0)
    return NULL;

  char* bodyCopy = (char*) kaAlloc(&swRest.kalloc, resp.bodyLen + 1);
  memcpy(bodyCopy, resp.body, resp.bodyLen);
  bodyCopy[resp.bodyLen] = 0;

  KjNode* mapTreeP = kjParse(swRest.kjsonP, bodyCopy);
  if (mapTreeP == NULL || mapTreeP->type != KjObject)
    return NULL;

  // Pull out the remote EntityMap's id and entityMap.
  KjNode* idP = kjLookup(mapTreeP, "id");
  if (idP != NULL && idP->type == KjString && remoteMapIdOut != NULL)
    *remoteMapIdOut = idP->value.s;

  KjNode* emObj = kjLookup(mapTreeP, "entityMap");
  if (emObj == NULL || emObj->type != KjObject)
    return NULL;

  // Synthesize { "id": <entityId> } entries — feeds the existing merge loop.
  KjNode* idArray = kjArray(swRest.kjsonP, NULL);
  for (KjNode* entryP = emObj->value.firstChildP; entryP != NULL; entryP = entryP->next)
  {
    if (entryP->name == NULL)
      continue;
    KjNode* synth = kjObject(swRest.kjsonP, NULL);
    kjChildAdd(synth, kjString(swRest.kjsonP, "id", entryP->name));
    kjChildAdd(idArray, synth);
  }

  return idArray;
}



// -----------------------------------------------------------------------------
//
// retrieveEntityFromCSR - GET /entities/{id} from a specific CSR.
//
// Used by entity-map pagination: the map records the source per entity,
// and the client's paged follow-up retrieves each entity from its
// recorded source (not via reg-cache matching, which could return a
// different set if registrations have changed since the map was built).
//
// Returns the expanded, storage-format entity tree on success, NULL on
// any failure (network error, non-2xx status, parse error).
//
static KjNode* retrieveEntityFromCSR(LdRegCacheItem* csr,
                                      const char*     entityId,
                                      const char*     ownAlias,
                                      const char*     remoteMapId)
{
  if (csr == NULL || csr->endpoint == NULL)
    return NULL;

  const char* base = csr->endpoint;
  const char* path = "/ngsi-ld/v1/entities/";
  const char* qs   = "?sysAttrs=true";
  int baseLen = strlen(base);
  int pathLen = strlen(path);
  int idLen   = strlen(entityId);
  int qsLen   = strlen(qs);
  char* url   = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + idLen + qsLen + 1);

  strcpy(url, base);
  strcpy(url + baseLen, path);
  strcpy(url + baseLen + pathLen, entityId);
  strcpy(url + baseLen + pathLen + idLen, qs);

  char*       respBody    = NULL;
  int         respBodyLen = 0;
  const char* errDetail   = NULL;

  // § 5.14.4.4: when paginating from a local EntityMap that has a
  // linkedMaps entry for this CSR, forward the corresponding remote map
  // URI as NGSILD-EntityMap so the CP serves from its frozen snapshot.
  SwRestKeyValue extraH;
  int            extraN = 0;
  char           mapHdr[160];
  if (remoteMapId != NULL && remoteMapId[0] != 0)
  {
    snprintf(mapHdr, sizeof(mapHdr), "/ngsi-ld/v1/entityMaps/%s", remoteMapId);
    extraH.key   = (char*) "NGSILD-EntityMap";
    extraH.value = mapHdr;
    extraN = 1;
  }

  int status = ldDistOpSendReceiveEx(csr, SwVerbGet, url, NULL, 0, ownAlias,
                                      (extraN > 0) ? &extraH : NULL, extraN,
                                      &errDetail, &respBody, &respBodyLen);
  if (status < 200 || status >= 300 || respBody == NULL || respBodyLen == 0)
    return NULL;

  KjNode* treeP = kjParse(swRest.kjsonP, respBody);
  if (treeP == NULL)
    return NULL;

  swldExpandTree(treeP, swNgsild.contextP, &swRest.kalloc);
  ldStripAtContext(treeP);
  apiAttrToStorageWrap(treeP);

  // § 4.5.5.2 — entity-level expiresAt cascades to each Attribute, with
  // attr-level values further in the future shortened to entity-level.
  ldExpiresAtPropagate(treeP);
  return treeP;
}



// -----------------------------------------------------------------------------
//
// mergeAttrsNonOverriding - graft src's attrs into dest, skipping conflicts.
//
// Used by the entity-map pagination path when one entity is built from
// multiple recorded sources (split mode). First-wins for (attrName, dsKey)
// collisions — matches the main query's split-mode merge behaviour (full
// § 4.5.5.3 timestamp comparison is overkill here since both sides were
// already present in the map's snapshot).
//
static void mergeAttrsNonOverriding(KjNode* destP, KjNode* srcP)
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
    if (destAttrP == NULL)
    {
      srcAttrP->next = NULL;
      kjChildAdd(destP, srcAttrP);
    }
    else
    {
      // Merge per dsKey — add dsKeys not already present
      KjNode* srcInstP = srcAttrP->value.firstChildP;
      while (srcInstP != NULL)
      {
        KjNode* nextInst = srcInstP->next;
        if (kjLookup(destAttrP, srcInstP->name) == NULL)
        {
          srcInstP->next = NULL;
          kjChildAdd(destAttrP, srcInstP);
        }
        srcInstP = nextInst;
      }
    }

    srcAttrP = nextSrcAttr;
  }
}



// -----------------------------------------------------------------------------
//
// buildQueryString - build the forwarded query string for broker-to-broker comm
//
// Reconstructs from raw URL params but strips options (keyValues/concise/
// simplified) and format — broker-to-broker always uses normalized format.
// Also strips local, entityMap, orderBy, collation (those are local concerns).
//
static const char* buildQueryString(void)
{
  char* qs = (char*) kaAlloc(&swRest.kalloc, 4096);
  int pos = 0;

  for (int i = 0; i < swRest.in.uriParamCount; i++)
  {
    const char* key = swRest.in.uriParamV[i].key;

    // Skip params that are local-only or affect output format
    if (strcmp(key, "options")   == 0) continue;
    if (strcmp(key, "format")    == 0) continue;
    if (strcmp(key, "local")     == 0) continue;
    if (strcmp(key, "orderBy")   == 0) continue;
    if (strcmp(key, "collation") == 0) continue;
    if (strcmp(key, "entityMap") == 0) continue;
    if (strcmp(key, "pick")      == 0) continue;
    if (strcmp(key, "omit")      == 0) continue;

    if (pos > 0) qs[pos++] = '&';
    int kLen = strlen(key);
    int vLen = strlen(swRest.in.uriParamV[i].value);
    strcpy(qs + pos, key);
    pos += kLen;
    qs[pos++] = '=';
    strcpy(qs + pos, swRest.in.uriParamV[i].value);
    pos += vLen;
  }
  qs[pos] = 0;
  return qs;
}



// -----------------------------------------------------------------------------
//
// buildSplitForwardQueryString - URL params for the split-mode forward.
//
// Emits only the filters that are guaranteed safe to forward when the
// receiver may hold partial entities: id, idPattern, type. q/geoQ/scopeQ
// are NOT forwarded — they may reference sharded attributes the receiver
// can't fully evaluate. No local=true: snapshot federation should be
// transitive (loop detection in § 5.12 keeps it bounded).
//
// Last-resort fallback: when the request carries none of id/idPattern/
// type, we have to send local=true to satisfy the receiver's too-wide-
// query check (this disables transitive fanout for that hop only).
//
static const char* buildSplitForwardQueryString(void)
{
  char* qs = (char*) kaAlloc(&swRest.kalloc, 4096);
  int   pos = 0;

  for (int i = 0; i < swRest.in.uriParamCount; i++)
  {
    const char* key = swRest.in.uriParamV[i].key;
    if (strcmp(key, "id")        != 0 &&
        strcmp(key, "idPattern") != 0 &&
        strcmp(key, "type")      != 0)
      continue;

    if (pos > 0) qs[pos++] = '&';
    int kLen = strlen(key);
    int vLen = strlen(swRest.in.uriParamV[i].value);
    strcpy(qs + pos, key); pos += kLen;
    qs[pos++] = '=';
    strcpy(qs + pos, swRest.in.uriParamV[i].value); pos += vLen;
  }

  if (pos == 0)
  {
    const char* fallback = "local=true";
    int fLen = strlen(fallback);
    memcpy(qs, fallback, fLen);
    pos = fLen;
  }

  qs[pos] = 0;
  return qs;
}



// -----------------------------------------------------------------------------
//
// getEntities -
//
// -----------------------------------------------------------------------------
//
// entityMapPaginate - GET /entities?entityMap=<mapId> (§ 5.2.39 / § 5.7.2.4)
//
// Frozen-snapshot pagination: the map fixes "where each entity is" at
// map-creation time; filters on the current request still re-evaluate
// against the current data, but the candidate set is locked.
//
static bool entityMapPaginate(void)
{
  Tenant* tP = (Tenant*) swNgsild.tenantP;
  if (tP == NULL || tP->entityMapStoreP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity map not found");
    return true;
  }

  LdEntityMap* mapP = ldEntityMapLookup((LdEntityMapStore*) tP->entityMapStoreP, swNgsild.entityMapId);
  if (mapP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity map '%s' not found or expired", swNgsild.entityMapId);
    return true;
  }

  // Build result array from map entries at [offset..offset+limit]
  KjNode* arrayP = kjArray(NULL, NULL);
  int offset = swNgsild.offset;
  int limit  = (swNgsild.limit > 0) ? swNgsild.limit : 20;
  int ix     = 0;
  int added  = 0;

  //
  // Each map entry records a list of sources that contribute to the
  // entity. Retrieve from each listed source — local "@none" via
  // db.entityRetrieve, remote via HTTP GET on the CSR identified by
  // regId. Do NOT consult the registration cache for routing — the
  // whole point of the map is a stable snapshot, immune to later
  // reg changes.
  //
  const char* ownAlias = ldCsourceAliasForTenant(tP->name, &swRest.kalloc);

  for (LdEntityMapEntry* entryP = mapP->head; entryP != NULL && added < limit; entryP = entryP->next)
  {
    if (ix < offset) { ix++; continue; }

    KjNode* mergedEntity = NULL;

    for (int s = 0; s < entryP->sourceCount; s++)
    {
      const char* src      = entryP->sourceIdV[s];
      KjNode*     partialP = NULL;

      if (strcmp(src, "@none") == 0)
      {
        db.entityRetrieve(tP, entryP->entityId, &partialP);
      }
      else if (tP->regCacheP != NULL)
      {
        LdRegCacheItem* csr = ldRegCacheItemLookup((LdRegCache*) tP->regCacheP, src);
        if (csr != NULL)
        {
          // § 5.14.4.4: forward the CP's own EntityMap id (from linkedMaps)
          // so the CP serves from its frozen snapshot.
          const char* remoteMapId = ldEntityMapLinkedMapLookup(mapP, src);
          partialP = retrieveEntityFromCSR(csr, entryP->entityId, ownAlias, remoteMapId);
        }
      }
      // Source not reachable / deleted / reg removed → skip silently;
      // any other source that still resolves still contributes.

      if (partialP == NULL)
        continue;

      if (mergedEntity == NULL)
        mergedEntity = partialP;
      else
        mergeAttrsNonOverriding(mergedEntity, partialP);
    }

    if (mergedEntity != NULL)
      kjChildAdd(arrayP, mergedEntity);
    // else: all recorded sources failed — skip, client gets fewer
    // results than requested (§ 5.5.9 allows this).

    ix++;
    added++;
  }

  // Filters on the assembled entities — § 5.7.2.4.
  applyResultFilters(arrayP);

  if (swNgsild.count)
  {
    char* countStr = (char*) kaAlloc(&swRest.kalloc, 32);
    snprintf(countStr, 32, "%d", mapP->entryCount);
    swRestOutHeaderAdd("NGSILD-Results-Count", countStr);
  }

  // § 5.5.9.1: emit prev/first for any non-first page; next/last when more remain.
  bool hasMore = (offset + added < mapP->entryCount);
  bool hasPrev = (offset > 0);
  if (hasMore || hasPrev)
  {
    char* link = (char*) kaAlloc(&swRest.kalloc, 1024);
    int   pos  = 0;
    if (hasPrev)
    {
      int prevOff = offset - limit;
      if (prevOff < 0) prevOff = 0;
      pos += snprintf(link + pos, 1024 - pos,
                      "</ngsi-ld/v1/entities?entityMap=%s&offset=0&limit=%d>; rel=\"first\"; type=\"application/json\", "
                      "</ngsi-ld/v1/entities?entityMap=%s&offset=%d&limit=%d>; rel=\"prev\"; type=\"application/json\"",
                      swNgsild.entityMapId, limit,
                      swNgsild.entityMapId, prevOff, limit);
    }
    if (hasMore)
    {
      if (pos > 0) pos += snprintf(link + pos, 1024 - pos, ", ");
      int lastOff = ((mapP->entryCount - 1) / limit) * limit;
      pos += snprintf(link + pos, 1024 - pos,
                      "</ngsi-ld/v1/entities?entityMap=%s&offset=%d&limit=%d>; rel=\"next\"; type=\"application/json\", "
                      "</ngsi-ld/v1/entities?entityMap=%s&offset=%d&limit=%d>; rel=\"last\"; type=\"application/json\"",
                      swNgsild.entityMapId, offset + limit, limit,
                      swNgsild.entityMapId, lastOff, limit);
    }
    swRestOutHeaderAdd("Link", link);
  }

  if (swNgsild.pickV != NULL || swNgsild.omitV != NULL)
  {
    for (KjNode* ep = arrayP->value.firstChildP; ep != NULL; ep = ep->next)
      ldPickOmit(ep, swNgsild.pickV, swNgsild.omitV);

    // Drop entities reduced to empty by pick — § 4.21 / § 5.7.2 are
    // silent on this case (spec only mandates "reduce to specified
    // members"). ETSI agreed in plenary to filter out empty objects
    // rather than return an array of `{}` placeholders that carry no
    // useful information for the client. ?count= will still include
    // the dropped ones — a count mismatch the spec is yet to address.
    if (swNgsild.pickV != NULL)
    {
      KjNode* ep = arrayP->value.firstChildP;
      KjNode* prev = NULL;
      while (ep != NULL)
      {
        KjNode* next = ep->next;
        if (ep->type == KjObject && ep->value.firstChildP == NULL)
          kjChildRemove(arrayP, ep);
        else
          prev = ep;
        ep = next;
      }
      (void) prev;
    }
  }
  else if (swNgsild.attrsV != NULL)
  {
    // § 6.4.3.2 deprecated `attrs` — attribute-only filter, preserves
    // id / type / scope / @context (unlike pick which can strip them).
    for (KjNode* ep = arrayP->value.firstChildP; ep != NULL; ep = ep->next)
      ldAttrsFilter(ep, swNgsild.attrsV);
  }

  applyLinkedQPostFilter(arrayP);

  if (swNgsild.join != NULL)
  {
    int level = (swNgsild.joinLevel > 0) ? swNgsild.joinLevel : 1;
    if      (strcmp(swNgsild.join, "flat")   == 0) ldLinkedEntitiesExpandArrayFlat(arrayP, level, tP);
    else if (strcmp(swNgsild.join, "inline") == 0) ldLinkedEntitiesExpandArrayInline(arrayP, level, tP);
  }

  swRest.out.responseTree = arrayP;
  return true;
}



// -----------------------------------------------------------------------------
//
// getEntities -
//
bool getEntities(void)
{
  //
  // Cross-parameter validation (limit=0 requires count, etc.) — must run
  // before any per-branch logic so a bad URL param can't get acted on.
  //
  if (ldParamsValidate())
    return true;

  //
  // § 6.3.22 / § 5.5.15 — snapshot-aware read. NGSILD-Snapshot header
  // routes the query to the named snapshot's frozen entity store and
  // forces local-only behaviour (no distop forwarding).
  //
  {
    bool seen = false;
    LdSnapshotCacheItem* snapItem = ldSnapshotItemFromHeader(&seen);
    if (seen)
    {
      if (snapItem == NULL) return true;            // 404 raised by helper
      return snapshotGetEntities(snapItem);
    }
  }

  //
  // § 6.4.3.2 Table 6.4.3.2-2: NGSILD-EntityMap request header is an
  // alternative to ?entityMap=<id>. Value is the EntityMap URI; extract
  // the id (last URL segment). URL param takes precedence if both present.
  //
  if (swNgsild.entityMapId == NULL)
  {
    for (int i = 0; i < swRest.in.httpHeaderCount; i++)
    {
      if (strcasecmp(swRest.in.httpHeaderV[i].key, "NGSILD-EntityMap") == 0)
      {
        const char* val = swRest.in.httpHeaderV[i].value;
        if (val != NULL && val[0] != 0)
        {
          const char* slash = strrchr(val, '/');
          swNgsild.entityMapId = (char*) ((slash != NULL && slash[1] != 0) ? slash + 1 : val);
        }
        break;
      }
    }
  }

  //
  // EntityMap-based pagination — separate flow, dedicated helper.
  //
  if (swNgsild.entityMapId != NULL)
    return entityMapPaginate();

  //
  // § 5.7.2.4: too-wide-query rejection. At least one filter must be
  // supplied. The spec lists type/attrs/q/georel/local; we additionally
  // accept id and idPattern since they bound the candidate set as
  // tightly (an explicit URI list is not a "too wide" query). ?entityMap=
  // bypasses (paginating an already-bounded map).
  //
  if (swNgsild.entityMapId == NULL)
  {
    bool hasType   = (swNgsild.typeV != NULL || swNgsild.typeExpr != NULL);
    bool hasAttrs  = (swNgsild.attrsV != NULL);
    bool hasQ      = (swNgsild.qExpr != NULL);
    bool hasGeo    = (swNgsild.georel != NULL);
    bool isLocal   = swNgsild.local;
    bool hasId     = (swNgsild.idV != NULL || swNgsild.idPattern != NULL);
    if (!hasType && !hasAttrs && !hasQ && !hasGeo && !isLocal && !hasId)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "too wide query: at least one of id, idPattern, type, attrs, q, georel, or local must be supplied");
      return true;
    }
  }

  //
  // Geo-query inter-parameter validation
  //
  bool hasGeorel      = (swNgsild.georel      != NULL);
  bool hasGeometry    = (swNgsild.geometry     != NULL);
  bool hasCoordinates = (swNgsild.coordinates  != NULL);
  bool hasGeoproperty = (swNgsild.geoproperty  != NULL);

  if (hasGeorel || hasGeometry || hasCoordinates)
  {
    if (!hasGeorel || !hasGeometry || !hasCoordinates)
    {
      char missing[128] = "";
      int  len          = 0;

      if (!hasGeorel)      len += snprintf(missing + len, sizeof(missing) - len, "georel");
      if (!hasGeometry)    len += snprintf(missing + len, sizeof(missing) - len, "%sgeometry",    len > 0 ? ", " : "");
      if (!hasCoordinates) len += snprintf(missing + len, sizeof(missing) - len, "%scoordinates", len > 0 ? ", " : "");

      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid geo-query", "incomplete geo-query: missing %s", missing);
      return true;
    }
  }
  else if (hasGeoproperty)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid geo-query", "geoproperty without georel, geometry, and coordinates");
    return true;
  }

  //
  // Build query filter from URL params
  //
  DbQueryFilter filter = {0};

  filter.idV       = swNgsild.idV;
  filter.idPattern = swNgsild.idPattern;
  filter.typeV     = swNgsild.typeV;
  filter.typeExpr  = swNgsild.typeExpr;
  filter.scopeExpr = swNgsild.scopeExpr;
  filter.qExpr     = swNgsild.qExpr;
  filter.geoRel      = swNgsild.geoRel;
  filter.geometry    = swNgsild.geometry;
  filter.coordinates = swNgsild.coordinates;
  filter.geoproperty = swNgsild.geoproperty ? swNgsild.geoproperty : swldExpand(swNgsild.contextP, "location", &swRest.kalloc, NULL, NULL);
  filter.limit     = (swNgsild.limit > 0) ? swNgsild.limit + 1 : 0;
  filter.offset   = swNgsild.offset;
  filter.count    = swNgsild.count;

  //
  // Determine split-entities mode: per-request param overrides global setting.
  // Split mode only activates when registrations actually match (checked below).
  //
  bool splitModeSetting = swNgsild.splitEntitiesSet ? swNgsild.splitEntitiesVal : ldSplitEntities;

  //
  // Query the local database (full filters for now — re-queried without
  // filters if split mode activates below)
  //
  KjNode* arrayP = NULL;
  int     r      = db.entityQuery((Tenant*) swNgsild.tenantP, &filter, &arrayP);

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error querying entities");
    return true;
  }

  //
  // Source-provenance map (§ 5.2.39) — built only when an EntityMap is
  // being created. Each entityId maps to the list of sources that
  // contributed to it: "@none" for local, a CSR regId otherwise. Without
  // this, paged follow-up via ?entityMap=<id> cannot route to the right
  // source for a given entity.
  //
  KjNode* srcMap = swNgsild.entityMapCreate ? kjObject(swRest.kjsonP, NULL) : NULL;
  srcMapStampLocalFrom(srcMap, arrayP);

  // Linked-maps tracker (§ 5.14.4.4) — KjObject keyed by CSR regId, value =
  // remote EntityMap id. Populated as each per-CSR forward returns its
  // own EntityMap; flushed into mapP after the local map is created.
  KjNode* linkedMapsTracker = swNgsild.entityMapCreate ? kjObject(swRest.kjsonP, NULL) : NULL;

  //
  // Distributed query: if registrations match and ?local=true is not set,
  // forward to matching CSRs and merge results.
  //
  // No-split mode: forward full query, merge + dedup
  // Split mode:    forward without filters, merge all attrs per entity,
  //                then apply filters post-assembly
  //
  if (swNgsild.local == false)
  {
    Tenant*           tP    = (Tenant*) swNgsild.tenantP;
    LdRegCacheItem**  matchV = NULL;
    int               matchN = 0;
    bool              splitMode = false;  // only true if splitModeSetting AND regs match

    // Match all four CSR modes with per-RegistrationInfo dispatch.
    // Each info entry is a separate (type, attr-set) coverage unit.
    // Processing order per § 4.3.6: exclusive → redirect → inclusive → auxiliary.
    //
    if (tP != NULL && tP->regCacheP != NULL)
    {
      LdRegMode modes[] = { LdRegModeExclusive, LdRegModeRedirect, LdRegModeInclusive, LdRegModeAuxiliary };
      const char* ownAlias = ldCsourceAliasForTenant(tP->name, &swRest.kalloc);
      char**      pickWanted = computeWantedAttrs(&swRest.kalloc);

      //
      // Phase 1 — match all 4 modes up front. First match triggers split-mode
      // activation (one-shot local re-query without filters). Then build per-CSR
      // forwarded URLs; phase 2 fires them all concurrently via ldDistOpSendMulti;
      // phase 3 walks results in mode order so dedup + split-mode merge match the
      // pre-existing sequential semantics.
      //
      LdRegCacheItem** modeMatchV[4] = { NULL, NULL, NULL, NULL };
      int              modeMatchN[4] = { 0, 0, 0, 0 };
      int              totalMatch     = 0;
      for (int m = 0; m < 4; m++)
      {
        modeMatchN[m] = ldRegCacheMatchForRetrieve((LdRegCache*) tP->regCacheP,
                                                   NULL,
                                                   splitModeSetting ? NULL : swNgsild.typeV,
                                                   modes[m], &modeMatchV[m]);
        totalMatch += modeMatchN[m];
      }

      if (totalMatch > 0 && splitModeSetting)
      {
        splitMode = true;
        DbQueryFilter splitFilter = {0};
        splitFilter.idV       = filter.idV;
        splitFilter.idPattern = filter.idPattern;
        splitFilter.limit     = 1000000;
        arrayP = NULL;
        db.entityQuery((Tenant*) swNgsild.tenantP, &splitFilter, &arrayP);
        srcMapStampLocalFrom(srcMap, arrayP);
      }

      const char* baseQs = (totalMatch > 0) ? (splitMode ? "" : buildQueryString()) : NULL;

      LdDistOpBatchItem*   items   = (LdDistOpBatchItem*)   kaAlloc(&swRest.kalloc, totalMatch * sizeof(LdDistOpBatchItem));
      LdDistOpBatchResult* results = (LdDistOpBatchResult*) kaAlloc(&swRest.kalloc, totalMatch * sizeof(LdDistOpBatchResult));
      int                  itemCount = 0;
      memset(results, 0, totalMatch * sizeof(LdDistOpBatchResult));

      for (int m = 0; m < 4; m++)
      {
        for (int i = 0; i < modeMatchN[m]; i++)
        {
          LdRegCacheItem* csr = modeMatchV[m][i];
          if (csr->endpoint == NULL) continue;
          if (ldDistOpCsrWouldLoop(csr, ownAlias)) continue;

          if (swNgsild.geoRel != NULL && ((LdRegCache*) tP->regCacheP)->csrGeoMatchFunc != NULL)
          {
            const char* prop = swNgsild.geoproperty;
            KjNode* csrGeoP = csr->locationP;
            if (prop != NULL)
            {
              if (strcmp(prop, "observationSpace") == 0 ||
                  strcmp(prop, "https://uri.etsi.org/ngsi-ld/observationSpace") == 0)
                csrGeoP = csr->observationSpaceP;
              else if (strcmp(prop, "operationSpace") == 0 ||
                       strcmp(prop, "https://uri.etsi.org/ngsi-ld/operationSpace") == 0)
                csrGeoP = csr->operationSpaceP;
            }
            if (!((LdRegCache*) tP->regCacheP)->csrGeoMatchFunc(csrGeoP, swNgsild.geoRel,
                                                                 swNgsild.geometry, swNgsild.coordinates))
              continue;
          }

          const char* fullQs;
          if (splitMode)
          {
            char** csrExports = csrUnionExports(csr, &swRest.kalloc);
            bool   skip       = false;
            const char* pickParam = intersectAndPick(csrExports, NULL, pickWanted, &swRest.kalloc, &skip);
            if (skip) continue;

            const char* splitBase = buildSplitForwardQueryString();
            int   bLen = strlen(splitBase), pLen = strlen(pickParam);
            char* combined = (char*) kaAlloc(&swRest.kalloc, bLen + pLen + 1);
            strcpy(combined, splitBase);
            if (pLen > 0) strcpy(combined + bLen, pickParam);
            fullQs = combined;
          }
          else
          {
            fullQs = baseQs;
            bool csrSkipped = false;

            for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
            {
              if (swNgsild.typeV != NULL)
              {
                bool typeMatch = false;
                for (LdRegEntityInfo* eiP = riP->entityInfoV; eiP != NULL; eiP = eiP->next)
                {
                  if (eiP->type == NULL) { typeMatch = true; break; }
                  for (int t = 0; swNgsild.typeV[t] != NULL; t++)
                    if (strcmp(eiP->type, swNgsild.typeV[t]) == 0) { typeMatch = true; break; }
                  if (typeMatch) break;
                }
                if (!typeMatch) continue;
              }

              bool        skip      = false;
              const char* pickParam = intersectAndPick(riP->propertyNamesV, riP->relationshipNamesV,
                                                        pickWanted, &swRest.kalloc, &skip);
              if (skip) { csrSkipped = true; break; }

              int   bLen = strlen(baseQs), pLen = strlen(pickParam);
              char* combined = (char*) kaAlloc(&swRest.kalloc, bLen + pLen + 1);
              strcpy(combined, baseQs);
              strcpy(combined + bLen, pickParam);
              fullQs = combined;
              break;
            }

            if (csrSkipped) continue;
          }

          const char* path    = swNgsild.entityMapCreate ? "/ngsi-ld/v1/entityMaps?"
                                                         : "/ngsi-ld/v1/entities?";
          int   baseLen = strlen(csr->endpoint);
          int   pathLen = strlen(path);
          int   qsLen   = strlen(fullQs);
          char* url     = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + qsLen + 1);
          strcpy(url, csr->endpoint);
          strcpy(url + baseLen, path);
          strcpy(url + baseLen + pathLen, fullQs);

          items[itemCount].csr     = csr;
          items[itemCount].url     = url;
          items[itemCount].body    = NULL;
          items[itemCount].bodyLen = 0;
          itemCount++;
        }
      }

      if (itemCount > 0)
      {
        ldDistOpSendMulti(items, itemCount, SwVerbGet, ownAlias, results);

        for (int i = 0; i < itemCount; i++)
        {
          LdRegCacheItem* csr    = items[i].csr;
          int             code   = results[i].statusCode;

          if (code < 200 || code >= 300) continue;
          if (results[i].responseBody == NULL || results[i].responseBodyLen == 0) continue;

          KjNode* remoteArray;

          if (swNgsild.entityMapCreate)
          {
            // § 5.14.4.4: response is a single EntityMap object. Pull out
            // remote map id + synthesise an array of { "id": <entityId> }
            // entries so the dedup loop below stays format-agnostic.
            KjNode* mapTreeP = kjParse(swRest.kjsonP, results[i].responseBody);
            if (mapTreeP == NULL || mapTreeP->type != KjObject) continue;

            KjNode* idP = kjLookup(mapTreeP, "id");
            if (linkedMapsTracker != NULL && idP != NULL && idP->type == KjString && csr->regId != NULL)
              kjChildAdd(linkedMapsTracker, kjString(swRest.kjsonP, csr->regId, idP->value.s));

            KjNode* emObj = kjLookup(mapTreeP, "entityMap");
            if (emObj == NULL || emObj->type != KjObject) continue;

            remoteArray = kjArray(swRest.kjsonP, NULL);
            for (KjNode* entryP = emObj->value.firstChildP; entryP != NULL; entryP = entryP->next)
            {
              if (entryP->name == NULL) continue;
              KjNode* synth = kjObject(swRest.kjsonP, NULL);
              kjChildAdd(synth, kjString(swRest.kjsonP, "id", entryP->name));
              kjChildAdd(remoteArray, synth);
            }
          }
          else
          {
            remoteArray = kjParse(swRest.kjsonP, results[i].responseBody);
            if (remoteArray != NULL) ldStripAtContext(remoteArray);
          }

          if (remoteArray == NULL || remoteArray->type != KjArray) continue;

          for (KjNode* remoteEntity = remoteArray->value.firstChildP; remoteEntity != NULL; )
          {
            KjNode* nextRemote = remoteEntity->next;

            KjNode* remoteIdP = kjLookup(remoteEntity, "id");
            if (remoteIdP == NULL || remoteIdP->type != KjString)
            {
              remoteEntity = nextRemote;
              continue;
            }

            KjNode* existingP = NULL;
            for (KjNode* ep = arrayP->value.firstChildP; ep != NULL; ep = ep->next)
            {
              KjNode* eidP = kjLookup(ep, "id");
              if (eidP != NULL && eidP->type == KjString && strcmp(eidP->value.s, remoteIdP->value.s) == 0)
              {
                existingP = ep;
                break;
              }
            }

            remoteEntity->next = NULL;
            swldExpandTree(remoteEntity, swNgsild.contextP, &swRest.kalloc);
            apiAttrToStorageWrap(remoteEntity);

            if (existingP == NULL)
            {
              kjChildAdd(arrayP, remoteEntity);
              srcMapAdd(srcMap, remoteIdP->value.s, csr->regId);
            }
            else if (splitMode)
            {
              srcMapAdd(srcMap, remoteIdP->value.s, csr->regId);
              for (KjNode* srcAttrP = remoteEntity->value.firstChildP; srcAttrP != NULL; )
              {
                KjNode* nextSrcAttr = srcAttrP->next;

                if (srcAttrP->name == NULL || srcAttrP->name[0] == '@' ||
                    strcmp(srcAttrP->name, "id") == 0 ||
                    strcmp(srcAttrP->name, "type") == 0 ||
                    srcAttrP->type != KjObject)
                {
                  srcAttrP = nextSrcAttr;
                  continue;
                }

                KjNode* destAttrP = kjLookup(existingP, srcAttrP->name);

                if (destAttrP == NULL)
                {
                  srcAttrP->next = NULL;
                  kjChildAdd(existingP, srcAttrP);
                }
                else
                {
                  for (KjNode* srcInstP = srcAttrP->value.firstChildP; srcInstP != NULL; )
                  {
                    KjNode* nextSrcInst = srcInstP->next;
                    KjNode* destInstP = kjLookup(destAttrP, srcInstP->name);

                    if (destInstP == NULL)
                    {
                      if (!ldDistInstanceIsExpired(srcInstP, swRest.requestStartTime))
                      {
                        srcInstP->next = NULL;
                        kjChildAdd(destAttrP, srcInstP);
                      }
                    }
                    else if (ldDistInstanceShouldReplace(destInstP, srcInstP, swRest.requestStartTime))
                    {
                      srcInstP->next = NULL;
                      kjChildReplace(destAttrP, destInstP, srcInstP);
                    }

                    srcInstP = nextSrcInst;
                  }
                }

                srcAttrP = nextSrcAttr;
              }
            }

            remoteEntity = nextRemote;
          }
        }
      }

      for (int m = 0; m < 4; m++)
        if (modeMatchV[m] != NULL) free(modeMatchV[m]);
      (void) matchV;
      (void) matchN;
    }

    // Split mode post-assembly filters (§ 5.7.2.4).
    if (splitMode)
      applyResultFilters(arrayP);
  }

  //
  // Sort by orderBy before pagination (§ 4.23)
  //
  if (swNgsild.orderByV != NULL && swNgsild.orderByCount > 0)
    ldOrderSort(arrayP, swNgsild.orderByV, swNgsild.orderByCount);

  //
  // Entity map: if entityMap=true, freeze the sorted entity IDs into a map
  // for consistent pagination. The map is stored per-tenant and its location
  // is returned via the NGSILD-EntityMap response header.
  //
  if (swNgsild.entityMapCreate)
  {
    Tenant* tP = (Tenant*) swNgsild.tenantP;

    if (tP != NULL && tP->entityMapStoreP != NULL)
    {
      // Purge expired maps first
      ldEntityMapPurgeExpired((LdEntityMapStore*) tP->entityMapStoreP);

      // Default lifetime: 5 minutes
      LdEntityMap* mapP = ldEntityMapCreate((LdEntityMapStore*) tP->entityMapStoreP,
                                             5ULL * 60 * 1000000000ULL, tP);

      //
      // Walk the sorted array, add each entity ID to the map along with
      // its provenance: look up the entity in srcMap and flatten the
      // KjArray of source strings into a char** for ldEntityMapAddEntry.
      //
      for (KjNode* entityP = arrayP->value.firstChildP; entityP != NULL; entityP = entityP->next)
      {
        KjNode* idP = kjLookup(entityP, "id");
        if (idP == NULL || idP->type != KjString)
          continue;

        KjNode* srcArr = (srcMap != NULL) ? kjLookup(srcMap, idP->value.s) : NULL;

        int n = 0;
        if (srcArr != NULL && srcArr->type == KjArray)
          for (KjNode* s = srcArr->value.firstChildP; s != NULL; s = s->next) n++;

        if (n == 0)
        {
          // Shouldn't happen for an entity that made it into arrayP, but
          // stay safe — fall back to "@none".
          const char* lone = "@none";
          ldEntityMapAddEntry(mapP, idP->value.s, &lone, 1);
        }
        else
        {
          const char** srcV = (const char**) kaAlloc(&swRest.kalloc, n * sizeof(char*));
          int i = 0;
          for (KjNode* s = srcArr->value.firstChildP; s != NULL; s = s->next)
            if (s->type == KjString)
              srcV[i++] = s->value.s;
          ldEntityMapAddEntry(mapP, idP->value.s, srcV, i);
        }
      }

      // Flush per-CSR linkedMaps tracker (§ 5.14.4.4) into the map.
      if (linkedMapsTracker != NULL)
      {
        for (KjNode* p = linkedMapsTracker->value.firstChildP; p != NULL; p = p->next)
        {
          if (p->name == NULL || p->type != KjString)
            continue;
          ldEntityMapAddLinkedMap(mapP, p->name, p->value.s);
        }
      }

      // Add NGSILD-EntityMap header with the map's URL
      char* mapUrl = (char*) kaAlloc(&swRest.kalloc, 128);
      snprintf(mapUrl, 128, "/ngsi-ld/v1/entityMaps/%s", mapP->mapId);

      swRestOutHeaderAdd("NGSILD-EntityMap", mapUrl);

      //
      // GET / POST /entityMaps (§ 6.34.3): return the EntityMap itself
      // (201 Created) instead of the matching entities array. rawResponse
      // lets ldEntityMapToTree's JSON flow unchanged through renderHook.
      //
      if (swNgsild.entityMapOnly)
      {
        swRest.out.responseTree   = ldEntityMapToTree(mapP);
        swRest.out.httpStatusCode = 201;
        swNgsild.rawResponse      = true;
        return true;
      }
    }
  }

  //
  // Add NGSILD-Results-Count header if count was requested
  //
  if (swNgsild.count)
  {
    char* countStr = (char*) kaAlloc(&swRest.kalloc, 32);
    snprintf(countStr, 32, "%ld", (long) filter.totalCount);

    swRestOutHeaderAdd("NGSILD-Results-Count", countStr);
  }

  //
  // Pagination: trim to limit and add Link header with next/prev
  //
  bool hasMore = ldPaginationTrim(arrayP, swNgsild.limit);
  ldPaginationLinkHeader(hasMore);

  //
  // Apply pick/omit attribute projection (or the legacy attrs alias)
  //
  if (swNgsild.pickV != NULL || swNgsild.omitV != NULL)
  {
    for (KjNode* entityP = arrayP->value.firstChildP; entityP != NULL; entityP = entityP->next)
      ldPickOmit(entityP, swNgsild.pickV, swNgsild.omitV);

    // Drop entities reduced to empty by pick — see the earlier
    // identical block; spec is silent, ETSI plenary chose to drop.
    if (swNgsild.pickV != NULL)
    {
      KjNode* ep = arrayP->value.firstChildP;
      while (ep != NULL)
      {
        KjNode* next = ep->next;
        if (ep->type == KjObject && ep->value.firstChildP == NULL)
          kjChildRemove(arrayP, ep);
        ep = next;
      }
    }
  }
  else if (swNgsild.attrsV != NULL)
  {
    for (KjNode* entityP = arrayP->value.firstChildP; entityP != NULL; entityP = entityP->next)
      ldAttrsFilter(entityP, swNgsild.attrsV);
  }

  // § 4.9 LinkedEntityRelation — post-filter when q contains a sub-q.
  applyLinkedQPostFilter(arrayP);

  // § 4.5.23 — linked-entity expansion of each result.
  if (swNgsild.join != NULL)
  {
    int level = (swNgsild.joinLevel > 0) ? swNgsild.joinLevel : 1;
    Tenant* tP = (Tenant*) swNgsild.tenantP;
    if      (strcmp(swNgsild.join, "flat")   == 0) ldLinkedEntitiesExpandArrayFlat(arrayP, level, tP);
    else if (strcmp(swNgsild.join, "inline") == 0) ldLinkedEntitiesExpandArrayInline(arrayP, level, tP);
  }

  swRest.out.responseTree = arrayP;
  return true;
}

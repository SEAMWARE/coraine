//
// FILE            getEntities.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                  // NULL
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // free
#include <string.h>                                  // strcmp, strlen, strcpy

#include "ktrace/kTrace.h"                           // KT_T
#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjParse.h"                           // kjParse
#include "kjson/kjBuilder.h"                         // kjArray, kjObject, kjChildAdd, kjChildRemove
#include "kjson/kjChildReplace.h"                    // kjChildReplace
#include "corRest/CorRestState.h"                      // corRest
#include "corRest/corRestClient.h"                     // CorRestClientRequest, corRestClientSend
#include "corRest/corRestOutHeader.h"                  // corRestOutHeaderAdd
#include "corJsonld/corLdExpand.h"                     // corLdExpand
#include "corJsonld/corLdExpandTree.h"                 // corLdExpandTree
#include "corJsonld/corLdCompact.h"                    // corLdCompact
#include "corJsonld/corLdInit.h"                       // corLdCoreContext
#include "corJsonld/corLdDownload.h"                   // corLdContextFromUrl
#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/ldParamsValidate.h"               // ldParamsValidate
#include "corNgsild/ldOrderSort.h"                    // ldOrderSort
#include "corNgsild/ldIsEntityKeyword.h"             // ldIsEntityKeyword
#include "kalloc/kaStrdup.h"                        // kaStrdup
#include "kjson/kjRender.h"                         // kjFastRender
#include "kjson/kjRenderSize.h"                     // kjFastRenderSize
#include "corNgsild/ldStripAtContext.h"              // ldStripAtContext
#include "corNgsild/ldExpiresAtPropagate.h"          // ldExpiresAtPropagate
#include "corRest/CorRestIn.h"                 // corAcceptParse, CorMimeType
#include "corNgsild/ldEntityMatch.h"                  // ldEntityMatchType, ldEntityMatchQ, ldEntityMatchScope
#include "corNgsild/ldDistMerge.h"                    // ldDistMergeSourceInto
#include "corNgsild/ldQAttrs.h"                       // ldQAttrs
#include "corNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem
#include "corNgsild/ldRegCache.h"                     // ldRegCacheMatchForQuery
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "corNgsild/ldTraceLevels.h"                  // LdTRegMatch
#include "corNgsild/ldDistOp.h"                       // ldDistOpCsrWouldLoop, ldDistOpForwardContext
#include "corNgsild/ldQRender.h"                      // ldQRender, ldCompactOrEncode
#include "corNgsild/LdEntityMap.h"                    // LdEntityMap, LdEntityMapStore
#include "corNgsild/ldEntityMap.h"                    // ldEntityMapCreate, ldEntityMapAddEntry, ldEntityMapToTree
#include "corNgsild/ldQParse.h"                       // ldQParse, ldQStripLinked
#include "corNgsild/LdTypeExpr.h"                     // ldTypeExprParse
#include "corNgsild/LdScopeExpr.h"                    // ldScopeExprParse
#include "corNgsild/LdGeoRel.h"                       // ldGeoRelParse

#include "db/DbDriver.h"                             // db, DB_OK
#include "db/dbExpiredEntities.h"                 // dbExpiredEntityFilter
#include "db/Tenant.h"                               // Tenant

#include "linkedEntities/ldLinkedEntities.h"         // ldLinkedEntitiesExpandArrayFlat / Inline

#include "serviceRoutines/ldSnapshotRead.h"          // ldSnapshotItemFromHeader, snapshotGetEntities

#include "coraineTraceLevels.h"                     // KtDistOpRequest

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
  // q-filter evaluation must follow the relationship to test the predicate,
  // so it uses the legacy fetch-by-id path (typedRemoteOnly=false); the
  // § 7.7.1 fan-out gate applies to ?join retrieval, not filter matching.
  return linkedFetchOne(entityId, NULL, false, entityPP, (Tenant*) userData);
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
  if (corNgsild.qExpr == NULL || !qHasLinked(corNgsild.qExpr))
    return;

  Tenant* tP = (Tenant*) corNgsild.tenantP;

  KjNode* entityP = arrayP->value.firstChildP;
  while (entityP != NULL)
  {
    KjNode* nextP = entityP->next;

    if (!ldEntityMatchQEx(entityP, corNgsild.qExpr, linkedFetcher, tP))
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

    if (keep && corNgsild.typeExpr != NULL)
    {
      KjNode* typeP = kjLookup(entityP, "type");
      if (!ldEntityMatchType(typeP, corNgsild.typeExpr))
        keep = false;
    }

    if (keep && corNgsild.qExpr != NULL)
    {
      if (!ldEntityMatchQEx(entityP, corNgsild.qExpr, linkedFetcher, (Tenant*) corNgsild.tenantP))
        keep = false;
    }

    if (keep && corNgsild.scopeExpr != NULL)
    {
      KjNode* scopeP = kjLookup(entityP, "scope");
      if (!ldEntityMatchScope(scopeP, corNgsild.scopeExpr))
        keep = false;
    }

    if (keep && corNgsild.geoRel != NULL && db.geoMatchFunc != NULL)
    {
      if (!db.geoMatchFunc(entityP, corNgsild.geoRel, corNgsild.geometry,
                           corNgsild.coordinates,
                           corNgsild.geoproperty ? corNgsild.geoproperty : "location"))
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
//   orderBy attrs                  ∪   (sort runs locally on the merged result, § 4.23 /
//                                       line 1651 — without these the merged entity
//                                       lacks the sort key and order is non-deterministic)
//
// Allocated in the request arena. Strings are borrowed where possible
// (pickV and ldQAttrs results are already in the arena), expanded on
// demand for the geo property short-name fallback.
//
static char** computeWantedAttrs(KAlloc* kaP)
{
  // `attrs` (the deprecated selection+projection alias) narrows the
  // forward exactly like pick does — the per-source projection is safe
  // (selection re-runs post-assembly), and without it the forward asks
  // the source for everything.
  char** projV = (corNgsild.pickV != NULL) ? corNgsild.pickV : corNgsild.attrsV;

  if (projV == NULL)
    return NULL;

  // Worst-case capacity: projV count + q-attr count + 2 (geo + geometry) + orderBy count
  int pickN = 0;
  while (projV[pickN] != NULL)
    pickN++;

  char** qV   = ldQAttrs(corNgsild.qExpr, kaP);
  int    qN   = 0;
  if (qV != NULL)
    while (qV[qN] != NULL)
      qN++;

  bool hasGeoQ        = (corNgsild.georel != NULL);
  bool acceptGeoJson  = (corAcceptParse(corRest.in.accept) == CorMimeGeoJson);

  int cap = pickN + qN + 2 + corNgsild.orderByCount;
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
    WANT_ADD(projV[i]);

  for (int i = 0; i < qN; i++)
    WANT_ADD(qV[i]);

  if (hasGeoQ)
  {
    const char* gp = corNgsild.geoproperty
                     ? corNgsild.geoproperty
                     : corLdExpand(corNgsild.contextP, "location", kaP, NULL, NULL);
    WANT_ADD(gp);
  }

  if (acceptGeoJson)
  {
    // The GeoJSON "geometry" comes from the geometryProperty, defaulting to
    // "location" (§ 5.3.3.2) — same default as the geoQ branch above. Without
    // an explicit param this was previously skipped, so a geo+json query with
    // a `pick` that did not list location forwarded a pick lacking it: the
    // Context Source then omitted location and the rendered geometry was null.
    const char* gmName = (corNgsild.geometryProperty != NULL) ? corNgsild.geometryProperty : "location";
    char* gmp = corLdExpand(corNgsild.contextP, gmName, kaP, NULL, NULL);
    WANT_ADD(gmp);
  }

  // orderBy attrs: ldOrderSort runs locally on the merged result, so every
  // CSR must return the attr values that will be sorted on, otherwise the
  // merged entity has nothing to compare and the sort order silently
  // collapses (or worse, depends on which slice arrived first).
  for (int i = 0; i < corNgsild.orderByCount; i++)
    WANT_ADD(corNgsild.orderByV[i].attrName);

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
// urlEncodeReserved - percent-encode the URL-reserved chars that would
// break a query string if emitted raw: '#' (fragment delimiter), '&'
// (param delimiter), '=' (key/value separator), '%' (escape lead),
// '?' (query lead). Other RFC 3986 reserved chars (':', '/', etc.) are
// permitted in query values per § 3.4 and we keep them readable (the URN
// shape `urn:ngsi-ld:Vehicle:X` stays compact).
//
// Used on the fallback "compaction returned the IRI unchanged" path,
// where the expanded IRI carries '#' from the fragment-style core
// context. Returns a freshly-allocated string from kaP.
//
static const char* urlEncodeReserved(const char* s, KAlloc* kaP)
{
  if (s == NULL) return "";
  int extra = 0;
  for (const char* p = s; *p; p++)
    if (*p == '#' || *p == '&' || *p == '=' || *p == '%' || *p == '?')
      extra += 2;  // 1 char → 3 chars

  if (extra == 0)
    return s;  // nothing to encode, return as-is

  char* out = (char*) kaAlloc(kaP, strlen(s) + extra + 1);
  char* w   = out;
  static const char hex[] = "0123456789ABCDEF";
  for (const char* p = s; *p; p++)
  {
    if (*p == '#' || *p == '&' || *p == '=' || *p == '%' || *p == '?')
    {
      *w++ = '%';
      *w++ = hex[(unsigned char)*p >> 4];
      *w++ = hex[(unsigned char)*p & 0xf];
    }
    else
      *w++ = *p;
  }
  *w = 0;
  return out;
}


// compactForUrl - compact an expanded IRI via the given context and make
// the result safe to embed in a URL query-string value. If the alias is
// found in `ctx`, returns the short name as-is (short names are URL-
// safe). If not, falls back to the expanded IRI and percent-encodes any
// URL-reserved chars.
//
// `ctx` MUST NOT be NULL — callers ensure that (e.g. CSR's forwardCtxP
// is initialised to core at registration time).
//
static const char* compactForUrl(CorLdContext* ctx, const char* iri, KAlloc* kaP)
{
  if (iri == NULL || iri[0] == 0)
    return "";
  const char* shorter = corLdCompact(ctx, iri);
  if (shorter == NULL || shorter == iri)
    return urlEncodeReserved(iri, kaP);
  // Compaction returned a new pointer — assume it's a short alias from
  // the context and URL-safe.
  return shorter;
}


static const char* buildPickParam(char** vec, KAlloc* kaP, CorLdContext* csrCtx)
{
  if (vec == NULL || vec[0] == NULL)
    return "";

  if (csrCtx == NULL) csrCtx = corLdCoreContext();

  int totalLen = 0;
  for (int i = 0; vec[i] != NULL; i++)
  {
    const char* c = compactForUrl(csrCtx, vec[i], kaP);
    totalLen += strlen(c) + 1;
  }
  totalLen += sizeof("id,type,scope,");  // upper bound on the appended suffix

  char* buf = (char*) kaAlloc(kaP, 6 + totalLen + 1);
  strcpy(buf, "&pick=");
  int pos = 6;
  for (int i = 0; vec[i] != NULL; i++)
  {
    if (pos > 6) buf[pos++] = ',';
    const char* n = compactForUrl(csrCtx, vec[i], kaP);
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
// exportV    : NULL-terminated array of expanded IRIs that the source
//              "exports". NULL means the source exports everything (no
//              restriction).
// wanted     : NULL = user wants every attr the source has, char** = the
//              IRI set the broker needs back
// kaP        : output allocator
// outSkipP   : *outSkipP set to true if user.pick was set AND the
//              intersection is empty (caller must skip this CSR)
//
// Returns the rendered "&pick=A,B,C" fragment, "" when no pick should
// be sent (source exports everything AND user wants everything).
//
static const char* intersectAndPick(char** exportV, char** wanted, KAlloc* kaP, bool* outSkipP, CorLdContext* csrCtx)
{
  *outSkipP = false;

  bool regRestricts = (exportV != NULL);

  if (wanted == NULL)
  {
    // User wants everything.
    if (!regRestricts)
      return "";  // exports everything → no pick

    // Reg restricts → forward exports as pick (avoids overquery on attrs the reg won't deliver)
    return buildPickParam(exportV, kaP, csrCtx);
  }

  // User has pick: intersect(wanted, exports).
  if (!regRestricts)
    return buildPickParam(wanted, kaP, csrCtx);  // exports all → forward wanted as-is

  int wantedN = 0;
  while (wanted[wantedN] != NULL)
    wantedN++;

  char** narrowed = (char**) kaAlloc(kaP, (wantedN + 1) * sizeof(char*));
  int    nN       = 0;

  for (int w = 0; w < wantedN; w++)
  {
    bool found = false;

    for (int a = 0; exportV[a] != NULL; a++)
      if (strcmp(wanted[w], exportV[a]) == 0) { found = true; break; }

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

  return buildPickParam(narrowed, kaP, csrCtx);
}



// -----------------------------------------------------------------------------
//
// csrUnionExports - union of all infoV[*].attributeNamesV
//
// Split-mode forwards are CSR-level (not per-RegistrationInfo), so the
// "exports" set the broker should narrow against is the union across
// every info entry of the CSR. Returns a NULL-terminated, deduped char**
// allocated in kaP, or NULL when the CSR has no restriction at all
// (every info entry has attributeNamesV == NULL).
//
static char** csrUnionExports(LdRegCacheItem* csr, KAlloc* kaP)
{
  // Worst-case capacity
  int cap = 0;
  bool anyRestricts = false;
  for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
  {
    if (riP->attributeNamesV != NULL)
      anyRestricts = true;
    for (int a = 0; riP->attributeNamesV != NULL && riP->attributeNamesV[a] != NULL; a++)
      cap++;
  }

  if (!anyRestricts)
    return NULL;

  char** out = (char**) kaAlloc(kaP, (cap + 1) * sizeof(char*));
  int    n   = 0;
  for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
  {
    for (int a = 0; riP->attributeNamesV != NULL && riP->attributeNamesV[a] != NULL; a++)
    {
      bool dup = false;
      for (int i = 0; i < n; i++)
        if (strcmp(out[i], riP->attributeNamesV[a]) == 0) { dup = true; break; }
      if (!dup) out[n++] = riP->attributeNamesV[a];
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

    KjNode* wrapperP = kjObject(corRest.kjsonP, curP->name);
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
    arrP = kjArray(corRest.kjsonP, entityId);
    kjChildAdd(srcMap, arrP);
  }

  for (KjNode* s = arrP->value.firstChildP; s != NULL; s = s->next)
  {
    if (s->type == KjString && strcmp(s->value.s, source) == 0)
      return;
  }

  kjChildAdd(arrP, kjString(corRest.kjsonP, NULL, source));
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
  char* url   = (char*) kaAlloc(&corRest.kalloc, baseLen + pathLen + idLen + qsLen + 1);

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
  CorRestKeyValue extraH;
  int            extraN = 0;
  char           mapHdr[160];
  if (remoteMapId != NULL && remoteMapId[0] != 0)
  {
    snprintf(mapHdr, sizeof(mapHdr), "/ngsi-ld/v1/entityMaps/%s", remoteMapId);
    extraH.key   = (char*) "NGSILD-EntityMap";
    extraH.value = mapHdr;
    extraN = 1;
  }

  KT_T(KtDistOpRequest, "forward: GET %s", url);

  int status = ldDistOpSendReceiveEx(csr, CorVerbGet, url, NULL, 0, ownAlias,
                                      (extraN > 0) ? &extraH : NULL, extraN,
                                      &errDetail, &respBody, &respBodyLen);
  if (status < 200 || status >= 300 || respBody == NULL || respBodyLen == 0)
    return NULL;

  KjNode* treeP = kjParse(corRest.kjsonP, respBody);
  if (treeP == NULL)
    return NULL;

  corLdExpandTree(treeP, corNgsild.contextP, &corRest.kalloc);
  ldStripAtContext(treeP);
  apiAttrToStorageWrap(treeP);

  // § 4.5.5.2 — entity-level expiresAt cascades to each Attribute, with
  // attr-level values further in the future shortened to entity-level.
  ldExpiresAtPropagate(treeP, corRest.kjsonP);
  return treeP;
}






// -----------------------------------------------------------------------------
//
// csrPinnedIdsParam - "&id=<id1>,<id2>" when the CSR pins specific entities
//
// A registration whose matching EntityInfo entries all carry a specific
// `id` only ever serves those entities — forwarding the bare type query
// would ask the source for EVERYTHING of that type. Narrow the forward
// with the pinned ids. Only when the client itself sent no id/idPattern
// (their filter passes through raw and must not be widened), and only
// when EVERY type-matching EntityInfo is id-specific (one id-less or
// idPattern entry means the source legitimately holds more).
//
static const char* csrPinnedIdsParam(LdRegCacheItem* csr, KAlloc* kaP)
{
  if (corNgsild.id != NULL && corNgsild.id[0] != 0)
    return "";
  if (corNgsild.idPattern != NULL && corNgsild.idPattern[0] != 0)
    return "";

  int   cap  = 512;
  char* buf  = (char*) kaAlloc(kaP, cap);
  int   pos  = 0;
  int   ids  = 0;

  for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
  {
    for (LdRegEntityInfo* eiP = riP->entityInfoV; eiP != NULL; eiP = eiP->next)
    {
      // type filter: only entityInfos the query can match
      if (corNgsild.typeV != NULL && eiP->type != NULL)
      {
        bool typeMatch = false;
        for (int t = 0; corNgsild.typeV[t] != NULL; t++)
          if (strcmp(eiP->type, corNgsild.typeV[t]) == 0) { typeMatch = true; break; }
        if (!typeMatch)
          continue;
      }

      if (eiP->id == NULL || eiP->idPatternList != NULL)
        return "";   // id-less or pattern-matched entry — can't narrow

      const char* v    = urlEncodeReserved(eiP->id, kaP);
      int         vLen = strlen(v);
      if (pos + vLen + 8 >= cap)
      {
        int   newCap = cap * 2 + vLen;
        char* nb     = (char*) kaAlloc(kaP, newCap);
        memcpy(nb, buf, pos);
        buf = nb;
        cap = newCap;
      }
      if (ids == 0) { memcpy(buf + pos, "&id=", 4); pos += 4; }
      else            buf[pos++] = ',';
      memcpy(buf + pos, v, vLen); pos += vLen;
      ids++;
    }
  }

  if (ids == 0)
    return "";

  buf[pos] = 0;
  return buf;
}



// -----------------------------------------------------------------------------
//
// buildQueryBodyFromQs - translate a forward query string into a § 5.2.23
//                        Query body (the queryBatch form of the same ask)
//
// The query string is already CSR-compacted (type/pick values rendered via
// the CSR's @context), so the body inherits correct short names. Handles
// the components the GET form emits: type, id, idPattern, q, pick (minus
// the id/type/scope keywords pick carries for projection-survival — the
// Query.attrs member never strips entity members).
//
static const char* buildQueryBodyFromQs(const char* qs, KAlloc* kaP)
{
  Kjson*  kjsonP = corRest.kjsonP;
  KjNode* bodyP  = kjObject(kjsonP, NULL);
  kjChildAdd(bodyP, kjString(kjsonP, "type", "Query"));

  char* types     = NULL;
  char* ids       = NULL;
  char* idPattern = NULL;
  char* q         = NULL;
  char* pick      = NULL;

  char* dup = kaStrdup(kaP, qs);
  char* sp  = NULL;
  for (char* tok = strtok_r(dup, "&", &sp); tok != NULL; tok = strtok_r(NULL, "&", &sp))
  {
    char* eq = strchr(tok, '=');
    if (eq == NULL) continue;
    *eq = 0;
    char* val = eq + 1;
    if      (strcmp(tok, "type")      == 0) types     = val;
    else if (strcmp(tok, "id")        == 0) ids       = val;
    else if (strcmp(tok, "idPattern") == 0) idPattern = val;
    else if (strcmp(tok, "q")         == 0) q         = val;
    else if (strcmp(tok, "pick")      == 0) pick      = val;
  }

  // entities[] — one selector per type (carrying id/idPattern when given).
  // With ids but no type, one selector per id.
  KjNode* entitiesP = kjArray(kjsonP, "entities");
  if (types != NULL)
  {
    char* tsp = NULL;
    for (char* t = strtok_r(types, ",", &tsp); t != NULL; t = strtok_r(NULL, ",", &tsp))
    {
      KjNode* selP = kjObject(kjsonP, NULL);
      kjChildAdd(selP, kjString(kjsonP, "type", t));
      if (ids != NULL)       kjChildAdd(selP, kjString(kjsonP, "id", ids));            // CSV is legal in the selector? No — id is a single URI; multiple ids → idPattern... keep first
      if (idPattern != NULL) kjChildAdd(selP, kjString(kjsonP, "idPattern", idPattern));
      kjChildAdd(entitiesP, selP);
    }
  }
  else if (ids != NULL)
  {
    char* isp = NULL;
    for (char* iv = strtok_r(ids, ",", &isp); iv != NULL; iv = strtok_r(NULL, ",", &isp))
    {
      KjNode* selP = kjObject(kjsonP, NULL);
      kjChildAdd(selP, kjString(kjsonP, "id", iv));
      kjChildAdd(entitiesP, selP);
    }
  }
  if (entitiesP->value.firstChildP != NULL)
    kjChildAdd(bodyP, entitiesP);

  if (q != NULL)
    kjChildAdd(bodyP, kjString(kjsonP, "q", q));

  if (pick != NULL)
  {
    KjNode* attrsP = kjArray(kjsonP, "attrs");
    char*   psp    = NULL;
    for (char* a = strtok_r(pick, ",", &psp); a != NULL; a = strtok_r(NULL, ",", &psp))
    {
      if (strcmp(a, "id") == 0 || strcmp(a, "type") == 0 || strcmp(a, "scope") == 0)
        continue;
      kjChildAdd(attrsP, kjString(kjsonP, NULL, a));
    }
    if (attrsP->value.firstChildP != NULL)
      kjChildAdd(bodyP, attrsP);
  }

  int   sz  = kjFastRenderSize(bodyP) + 1;
  char* buf = (char*) kaAlloc(kaP, sz);
  kjFastRender(bodyP, buf);
  return buf;
}



// -----------------------------------------------------------------------------
//
// buildQueryString - build the forwarded query string for broker-to-broker comm
//
// Reconstructs from raw URL params but strips options (keyValues/concise/
// simplified) and format — broker-to-broker always uses normalized format.
// Also strips local, entityMap, orderBy, collation, pick, omit (local concerns
// or per-CSR computed via intersectAndPick).
//
// JSON-LD alias-bearing params (type, attrs, q, geoproperty,
// geometryProperty) bypass the raw uriParamV passthrough: their raw values
// are CLIENT-context shorts, but the receiver parses with the @context the
// forward carries — so we emit the expanded IRI compacted via the forward
// context (compactForUrl; ldQRender for q's embedded attribute terms), the
// same algorithm buildSplitForwardQueryString uses.
//
static const char* buildQueryString(CorLdContext* csrCtx)
{
  if (csrCtx == NULL) csrCtx = corLdCoreContext();

  char* qs = (char*) kaAlloc(&corRest.kalloc, 4096);
  int   pos = 0;

  // type — alias-bearing → emit from corNgsild.typeV via CSR ctx
  if (corNgsild.typeV != NULL && corNgsild.typeV[0] != NULL)
  {
    strcpy(qs + pos, "type="); pos += 5;
    for (int i = 0; corNgsild.typeV[i] != NULL; i++)
    {
      if (i > 0) qs[pos++] = ',';
      const char* v = compactForUrl(csrCtx, corNgsild.typeV[i], &corRest.kalloc);
      int vLen = strlen(v);
      strcpy(qs + pos, v); pos += vLen;
    }
  }

  // q — its attribute terms are aliases too; re-render the parsed
  // expression via the forward context (uncompactable IRIs %-encoded).
  //
  // attrs (deprecated) is NEVER forwarded — its projection half already
  // rides the per-CSR pick (computeWantedAttrs); its selection half
  // ("at least one of the listed attributes exists") becomes attr-EXISTS
  // terms OR-ed together and AND-ed onto the initial q:
  //     q=(<initial q>);(a|b|c)     — or just q=a|b|c without a q.
  //
  char* qRendered = (corNgsild.qExpr != NULL) ? ldQRender(corNgsild.qExpr, csrCtx, &corRest.kalloc, false) : NULL;
  if (qRendered != NULL && qRendered[0] == 0) qRendered = NULL;

  char* attrsExists = NULL;
  if (corNgsild.attrsV != NULL && corNgsild.attrsV[0] != NULL)
  {
    int cap = 2;
    for (int i = 0; corNgsild.attrsV[i] != NULL; i++)
      cap += strlen(corNgsild.attrsV[i]) * 3 + 1;
    attrsExists = (char*) kaAlloc(&corRest.kalloc, cap);
    int apos = 0;
    for (int i = 0; corNgsild.attrsV[i] != NULL; i++)
    {
      if (i > 0) attrsExists[apos++] = '|';
      const char* v = ldCompactOrEncode(corNgsild.attrsV[i], csrCtx, &corRest.kalloc, false);
      strcpy(attrsExists + apos, v);
      apos += strlen(v);
    }
    attrsExists[apos] = 0;
  }

  if (qRendered != NULL || attrsExists != NULL)
  {
    if (pos > 0) qs[pos++] = '&';
    strcpy(qs + pos, "q="); pos += 2;
    if (qRendered != NULL && attrsExists != NULL)
      pos += sprintf(qs + pos, "(%s);(%s)", qRendered, attrsExists);
    else if (qRendered != NULL)
      pos += sprintf(qs + pos, "%s", qRendered);
    else
      pos += sprintf(qs + pos, "%s", attrsExists);
  }

  // geoproperty — alias-bearing (corNgsild.geoproperty is already expanded)
  if (corNgsild.geoproperty != NULL && corNgsild.geoproperty[0] != 0)
  {
    if (pos > 0) qs[pos++] = '&';
    strcpy(qs + pos, "geoproperty="); pos += 12;
    const char* v = compactForUrl(csrCtx, corNgsild.geoproperty, &corRest.kalloc);
    int vLen = strlen(v);
    strcpy(qs + pos, v); pos += vLen;
  }

  // All other URL params: forward raw (id, idPattern, scope, scopeQ,
  // georel, geometry, coordinates, timerel, timeAt, lang, csf, …).
  for (int i = 0; i < corRest.in.uriParamCount; i++)
  {
    const char* key = corRest.in.uriParamV[i].key;

    // Skip params that are local-only, output-format, per-CSR computed, or
    // handled above with alias-aware emission.
    if (strcmp(key, "options")          == 0) continue;
    if (strcmp(key, "sysAttrs")         == 0) continue;  // re-emitted below from the parsed flag
    if (strcmp(key, "format")           == 0) continue;
    if (strcmp(key, "local")            == 0) continue;
    if (strcmp(key, "orderBy")          == 0) continue;
    if (strcmp(key, "collation")        == 0) continue;
    if (strcmp(key, "entityMap")        == 0) continue;
    if (strcmp(key, "pick")             == 0) continue;
    if (strcmp(key, "omit")             == 0) continue;
    if (strcmp(key, "type")             == 0) continue;  // handled above
    if (strcmp(key, "attrs")            == 0) continue;  // deprecated — replaced by pick + q above
    if (strcmp(key, "q")                == 0) continue;  // handled above (ldQRender)
    if (strcmp(key, "geoproperty")      == 0) continue;  // handled above
    if (strcmp(key, "geometryProperty") == 0) continue;  // handled below

    if (pos > 0) qs[pos++] = '&';
    int kLen = strlen(key);
    int vLen = strlen(corRest.in.uriParamV[i].value);
    strcpy(qs + pos, key); pos += kLen;
    qs[pos++] = '=';
    strcpy(qs + pos, corRest.in.uriParamV[i].value); pos += vLen;
  }

  //
  // sysAttrs. This is the NON-split forward, where nothing is assembled from
  // several versions and § 4.5.5.3 never runs - so the sources are asked for
  // System Attributes only when the CLIENT wants them in the response.
  //
  // Emitted from the parsed flag rather than passed through raw, because the
  // client has two spellings for it (`sysAttrs=true` and `options=sysAttrs`)
  // and `options` is not forwarded - so the raw route honoured one spelling
  // and silently dropped the other.
  //
  if (corNgsild.sysAttrs)
  {
    if (pos > 0) qs[pos++] = '&';
    strcpy(qs + pos, "sysAttrs=true"); pos += 13;
  }

  // geometryProperty — alias-bearing but stored raw (client short). Expand
  // via the request's context, then compact via the CSR's.
  if (corNgsild.geometryProperty != NULL && corNgsild.geometryProperty[0] != 0)
  {
    if (pos > 0) qs[pos++] = '&';
    strcpy(qs + pos, "geometryProperty="); pos += 17;
    char* expanded = corLdExpand(corNgsild.contextP, corNgsild.geometryProperty, &corRest.kalloc, NULL, NULL);
    const char* v  = compactForUrl(csrCtx, expanded, &corRest.kalloc);
    int vLen = strlen(v);
    strcpy(qs + pos, v); pos += vLen;
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
static const char* buildSplitForwardQueryString(CorLdContext* csrCtx)
{
  char* qs = (char*) kaAlloc(&corRest.kalloc, 4096);
  int   pos = 0;

  if (csrCtx == NULL) csrCtx = corLdCoreContext();

  // First pass: prefer broker state (corNgsild.{typeV,id,idPattern}) which
  // is the parsed, JSON-LD-expanded form. compactForUrl renders each
  // value via the CSR's @context, falling back to a URL-encoded
  // expanded IRI when the CSR has no alias for the term.
  //
  // Why not the raw `corRest.in.uriParamV[]` like buildQueryString does?
  // Because raw URL params are short names from the CLIENT's @context.
  // A short name has no inherent meaning — it's an alias key into a
  // context. The client's `type=Vehicle` is the short for IRI X in the
  // client's context; the CSR's context might alias X as `Auto`, and
  // the *broker's job* on the forward is to emit whatever short the
  // CSR will recognise. Going via the canonical IRI (which broker state
  // already holds expanded) is the only way to produce CSR-correct
  // shorts. POST queryBatch routes its selectors through corNgsild
  // (ldQueryBodyToParams) but never into uriParamV; this path covers
  // both GET and POST uniformly.
  if (corNgsild.typeV != NULL && corNgsild.typeV[0] != NULL)
  {
    strcpy(qs + pos, "type="); pos += 5;
    for (int i = 0; corNgsild.typeV[i] != NULL; i++)
    {
      if (i > 0) qs[pos++] = ',';
      const char* v = compactForUrl(csrCtx, corNgsild.typeV[i], &corRest.kalloc);
      int vLen = strlen(v);
      strcpy(qs + pos, v); pos += vLen;
    }
  }

  if (corNgsild.id != NULL && corNgsild.id[0] != 0)
  {
    if (pos > 0) qs[pos++] = '&';
    strcpy(qs + pos, "id="); pos += 3;
    // id is a URI (not a JSON-LD alias) — compaction doesn't apply.
    // URL-encode only the chars that would break the query string.
    const char* v = urlEncodeReserved(corNgsild.id, &corRest.kalloc);
    int vLen = strlen(v);
    strcpy(qs + pos, v); pos += vLen;
  }

  if (corNgsild.idPattern != NULL && corNgsild.idPattern[0] != 0)
  {
    if (pos > 0) qs[pos++] = '&';
    strcpy(qs + pos, "idPattern="); pos += 10;
    const char* v = urlEncodeReserved(corNgsild.idPattern, &corRest.kalloc);
    int vLen = strlen(v);
    strcpy(qs + pos, v); pos += vLen;
  }

  if (pos == 0)
  {
    const char* fallback = "local=true";
    int fLen = strlen(fallback);
    memcpy(qs, fallback, fLen);
    pos = fLen;
  }

  //
  // sysAttrs=true is what makes § 4.5.5.3 rule 3 work at all.
  //
  // When the same (attrName, datasetId) arrives from several sources and NO
  // candidate carries observedAt, the tiebreaker is the newest modifiedAt.
  // modifiedAt is a System Attribute: a source only emits it when asked. The
  // forward used to omit the flag, so every forwarded instance arrived with
  // modifiedAt absent - read as 0 by ldDistInstanceShouldReplace, which then
  // kept whichever instance happened to be merged first. Rule 3 was silently
  // inoperative on this path, and it is the ONLY rule for the (common) case
  // of attributes without observedAt.
  //
  // The retrieve-one path has always sent it (getEntity.c). The flag does not
  // leak into the response: createdAt/modifiedAt are stripped at render time
  // unless the CLIENT asked for sysAttrs.
  //
  // It is appended AFTER the local=true fallback above, which keys off pos==0
  // - an unconditional param here would suppress that fallback entirely.
  //
  const char* sysAttrs = "&sysAttrs=true";
  int         saLen    = strlen(sysAttrs);
  memcpy(qs + pos, sysAttrs, saLen);
  pos += saLen;

  qs[pos] = 0;
  return qs;
}



// -----------------------------------------------------------------------------
//
// getEntities -
//
// -----------------------------------------------------------------------------
//
// bindEntityMapFilters - § 9 "same parameters" enforcement on EntityMap reuse.
//
// A filter param present at map creation may be re-sent with the SAME value or
// omitted; a DIFFERENT value, or a filter NOT used at creation, is rejected
// with 400 BadRequestData. (Modify / introduce are spec-clear violations of
// "shall use the same parameters"; allowing the OMIT case is the lenient
// direction, pending spec-doubt #96.) An omitted bound filter is re-parsed
// into corNgsild so applyResultFilters re-applies it live — an entity whose
// value has drifted out of the bound `q` since map creation still drops.
//
// Returns true and raises the 400 on a conflict; false otherwise.
//
static bool bindEntityMapFilters(LdEntityMap* mapP)
{
  KAlloc* kaP = &corRest.kalloc;

  struct { const char* name; const char* req; const char* bound; } f[] = {
    { "type",        corNgsild.type,        mapP->boundType        },
    { "q",           corNgsild.q,           mapP->boundQ           },
    { "scopeQ",      corNgsild.scopeQ,      mapP->boundScopeQ      },
    { "georel",      corNgsild.georel,      mapP->boundGeorel      },
    { "geometry",    corNgsild.geometry,    mapP->boundGeometry    },
    { "coordinates", corNgsild.coordinates, mapP->boundCoordinates },
    { "geoproperty", corNgsild.geoproperty, mapP->boundGeoproperty }
  };

  for (int i = 0; i < (int) (sizeof(f) / sizeof(f[0])); i++)
  {
    if (f[i].req == NULL)
      continue;
    if (f[i].bound == NULL)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "URL parameter '%s' was not part of the query that created entity map '%s'",
              f[i].name, corNgsild.entityMapId);
      return true;
    }
    if (strcmp(f[i].req, f[i].bound) != 0)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "URL parameter '%s' differs from the query that created entity map '%s'",
              f[i].name, corNgsild.entityMapId);
      return true;
    }
  }

  // Omitted bound filters → re-apply (parse into corNgsild for applyResultFilters).
  if (corNgsild.type == NULL && mapP->boundType != NULL)
  {
    corNgsild.type     = mapP->boundType;
    corNgsild.typeExpr = ldTypeExprParse(mapP->boundType, kaP);
  }
  if (corNgsild.q == NULL && mapP->boundQ != NULL)
  {
    corNgsild.q     = mapP->boundQ;
    corNgsild.qExpr = ldQParse(mapP->boundQ, kaP);
  }
  if (corNgsild.scopeQ == NULL && mapP->boundScopeQ != NULL)
  {
    corNgsild.scopeQ    = mapP->boundScopeQ;
    corNgsild.scopeExpr = ldScopeExprParse(mapP->boundScopeQ, kaP);
  }
  if (corNgsild.georel == NULL && mapP->boundGeorel != NULL)
  {
    corNgsild.georel      = mapP->boundGeorel;
    corNgsild.geoRel      = ldGeoRelParse(mapP->boundGeorel, kaP);
    corNgsild.geometry    = mapP->boundGeometry;
    corNgsild.coordinates = mapP->boundCoordinates;
    if (mapP->boundGeoproperty != NULL)
      corNgsild.geoproperty = mapP->boundGeoproperty;
  }

  return false;
}



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
  Tenant* tP = (Tenant*) corNgsild.tenantP;
  if (tP == NULL || tP->entityMapStoreP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity map not found");
    return true;
  }

  LdEntityMap* mapP = ldEntityMapLookup((LdEntityMapStore*) tP->entityMapStoreP, corNgsild.entityMapId);
  if (mapP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity map '%s' not found or expired", corNgsild.entityMapId);
    return true;
  }

  // § 9 same-parameters: reject a modified / newly-introduced filter; default
  // an omitted bound filter so it is re-applied live (see bindEntityMapFilters).
  if (bindEntityMapFilters(mapP))
    return true;

  // Build result array from map entries at [offset..offset+limit]
  KjNode* arrayP = kjArray(corRest.kjsonP, NULL);
  int offset = corNgsild.offset;
  int limit  = (corNgsild.limit > 0) ? corNgsild.limit : 20;
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
  const char* ownAlias = ldCsourceAliasForTenant(tP->name, &corRest.kalloc);

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
      {
        //
        // The same § 4.5.5.3 merge the unpaginated query does. It used to be a
        // local first-wins graft on the grounds that both versions came out of
        // the map's snapshot anyway - but "which value does this attribute
        // have" must not depend on whether the client paginated, and the
        // retrieve this path uses (retrieveEntityFromCSR) already asks for
        // sysAttrs, so the real rule has everything it needs.
        //
        ldDistMergeSourceInto(mergedEntity, partialP, corRest.requestStartTime, corRest.kjsonP, false);
      }
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

  if (corNgsild.count)
  {
    char* countStr = (char*) kaAlloc(&corRest.kalloc, 32);
    snprintf(countStr, 32, "%d", mapP->entryCount);
    corRestOutHeaderAdd("NGSILD-Results-Count", countStr);
  }

  // § 5.5.9.1: emit prev/first for any non-first page; next/last when more remain.
  bool hasMore = (offset + added < mapP->entryCount);
  bool hasPrev = (offset > 0);
  if (hasMore || hasPrev)
  {
    char* link = (char*) kaAlloc(&corRest.kalloc, 1024);
    int   pos  = 0;
    // § 6.4.7.2: the Link "type" attribute mirrors the original request's
    // media type, not a fixed value (same rule as ldPaginationLinkHeader).
    const char* mt = ldPaginationMediaType();
    if (hasPrev)
    {
      int prevOff = offset - limit;
      if (prevOff < 0) prevOff = 0;
      pos += snprintf(link + pos, 1024 - pos,
                      "</ngsi-ld/v1/entities?entityMap=%s&offset=0&limit=%d>; rel=\"first\"; type=\"%s\", "
                      "</ngsi-ld/v1/entities?entityMap=%s&offset=%d&limit=%d>; rel=\"prev\"; type=\"%s\"",
                      corNgsild.entityMapId, limit, mt,
                      corNgsild.entityMapId, prevOff, limit, mt);
    }
    if (hasMore)
    {
      if (pos > 0) pos += snprintf(link + pos, 1024 - pos, ", ");
      int lastOff = ((mapP->entryCount - 1) / limit) * limit;
      pos += snprintf(link + pos, 1024 - pos,
                      "</ngsi-ld/v1/entities?entityMap=%s&offset=%d&limit=%d>; rel=\"next\"; type=\"%s\", "
                      "</ngsi-ld/v1/entities?entityMap=%s&offset=%d&limit=%d>; rel=\"last\"; type=\"%s\"",
                      corNgsild.entityMapId, offset + limit, limit, mt,
                      corNgsild.entityMapId, lastOff, limit, mt);
    }
    corRestOutHeaderAdd("Link", link);
  }

  if (corNgsild.pickV != NULL || corNgsild.omitV != NULL)
  {
    for (KjNode* ep = arrayP->value.firstChildP; ep != NULL; ep = ep->next)
      ldPickOmit(ep, corNgsild.pickV, corNgsild.omitV);

    // Drop entities reduced to empty by pick — § 4.21 / § 5.7.2 are
    // silent on this case (spec only mandates "reduce to specified
    // members"). ETSI agreed in plenary to filter out empty objects
    // rather than return an array of `{}` placeholders that carry no
    // useful information for the client. ?count= will still include
    // the dropped ones — a count mismatch the spec is yet to address.
    if (corNgsild.pickV != NULL)
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
  else if (corNgsild.attrsV != NULL)
  {
    // § 6.4.3.2 deprecated `attrs` — attribute SELECTION + projection:
    // an entity carrying NONE of the listed attributes does not match
    // the query at all. Project each entity, then drop the ones left
    // with no attributes (only keywords like id/type/scope remain).
    KjNode* ep   = arrayP->value.firstChildP;
    KjNode* prev = NULL;
    while (ep != NULL)
    {
      KjNode* next = ep->next;
      ldAttrsFilter(ep, corNgsild.attrsV);

      bool hasAttr = false;
      for (KjNode* cP = ep->value.firstChildP; cP != NULL; cP = cP->next)
      {
        if (cP->name != NULL && !ldIsEntityKeyword(cP->name)) { hasAttr = true; break; }
      }
      if (!hasAttr)
        kjChildRemove(arrayP, ep);
      else
        prev = ep;
      ep = next;
    }
    (void) prev;
  }

  applyLinkedQPostFilter(arrayP);

  if (corNgsild.join != NULL)
  {
    int level = (corNgsild.joinLevel > 0) ? corNgsild.joinLevel : 1;
    if      (strcmp(corNgsild.join, "flat")   == 0) ldLinkedEntitiesExpandArrayFlat(arrayP, level, tP);
    else if (strcmp(corNgsild.join, "inline") == 0) ldLinkedEntitiesExpandArrayInline(arrayP, level, tP);
  }

  corRest.out.responseTree = arrayP;
  return true;
}



// -----------------------------------------------------------------------------
//
// geoJsonGeomProtectSetup - for a geo+json response, mark the selected geometry
// GeoProperty (geometryProperty, default "location", as an expanded IRI) so the
// pick/omit/attrs projection keeps it — ldToGeoJson needs it to build the
// "geometry" field even when the projection would otherwise drop it
// (§ 5.3.3.2: geometry is selected from the stored entity, independent of the
// member projection). geoJsonGeomForced records whether the user's projection
// would have dropped it, so ldToGeoJson prunes it from "properties".
//
static void geoJsonGeomProtectSetup(void)
{
  if (corAcceptParse(corRest.in.accept) != CorMimeGeoJson)
    return;

  CorLdContext* ctxP   = (corNgsild.contextP != NULL) ? corNgsild.contextP : corLdCoreContext();
  const char*  gmName = (corNgsild.geometryProperty != NULL) ? corNgsild.geometryProperty : "location";
  char*        gmIri  = corLdExpand(ctxP, gmName, &corRest.kalloc, NULL, NULL);
  if (gmIri == NULL)
    gmIri = (char*) gmName;

  corNgsild.geometryPropertyExpanded = gmIri;

  bool wanted = true;
  if (corNgsild.pickV != NULL)
  {
    wanted = false;
    for (int i = 0; corNgsild.pickV[i] != NULL; i++)
      if (strcmp(corNgsild.pickV[i], gmIri) == 0) { wanted = true; break; }
  }
  else if (corNgsild.omitV != NULL)
  {
    for (int i = 0; corNgsild.omitV[i] != NULL; i++)
      if (strcmp(corNgsild.omitV[i], gmIri) == 0) { wanted = false; break; }
  }
  else if (corNgsild.attrsV != NULL)
  {
    wanted = false;
    for (int i = 0; corNgsild.attrsV[i] != NULL; i++)
      if (strcmp(corNgsild.attrsV[i], gmIri) == 0) { wanted = true; break; }
  }

  corNgsild.geoJsonGeomForced = !wanted;
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

  // geo+json: protect the geometry GeoProperty through the member projection.
  geoJsonGeomProtectSetup();

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
  if (corNgsild.entityMapId == NULL)
  {
    for (int i = 0; i < corRest.in.httpHeaderCount; i++)
    {
      if (strcasecmp(corRest.in.httpHeaderV[i].key, "NGSILD-EntityMap") == 0)
      {
        const char* val = corRest.in.httpHeaderV[i].value;
        if (val != NULL && val[0] != 0)
        {
          const char* slash = strrchr(val, '/');
          corNgsild.entityMapId = (char*) ((slash != NULL && slash[1] != 0) ? slash + 1 : val);
        }
        break;
      }
    }
  }

  //
  // EntityMap-based pagination — separate flow, dedicated helper.
  //
  if (corNgsild.entityMapId != NULL)
    return entityMapPaginate();

  //
  // § 5.7.2.4: too-wide-query rejection. At least one filter must be
  // supplied. The spec lists type/attrs/q/georel/local; we additionally
  // accept id and idPattern since they bound the candidate set as
  // tightly (an explicit URI list is not a "too wide" query). ?entityMap=
  // bypasses (paginating an already-bounded map).
  //
  if (corNgsild.entityMapId == NULL)
  {
    bool hasType   = (corNgsild.typeV != NULL || corNgsild.typeExpr != NULL);
    bool hasAttrs  = (corNgsild.attrsV != NULL);
    bool hasQ      = (corNgsild.qExpr != NULL);
    bool hasGeo    = (corNgsild.georel != NULL);
    bool isLocal   = corNgsild.local;
    bool hasId     = (corNgsild.idV != NULL || corNgsild.idPattern != NULL);
    if (!hasType && !hasAttrs && !hasQ && !hasGeo && !isLocal && !hasId)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Query Too Broad",
              "too wide query: at least one of id, idPattern, type, attrs, q, georel, or local must be supplied");
      return true;
    }
  }

  //
  // § 4.5.23 / § 5.7 — a q sub-query over a Relationship (q=rel{...}) FOLLOWS
  // the link, and that follow-depth is bounded by joinLevel (default 1). A q
  // nested deeper than joinLevel cannot be evaluated → BadRequestData. (The
  // spec ties joinLevel to `join`; applying the same limit to a q sub-query is
  // our reading — spec-doubt #104.)
  //
  {
    int qDepth = (corNgsild.qExpr != NULL) ? corNgsild.qExpr->linkedDepth : 0;
    int jLevel = (corNgsild.joinLevel > 0) ? corNgsild.joinLevel : 1;
    if (qDepth > jLevel)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Query",
              "q linked sub-query nests %d level(s) deep, exceeding joinLevel %d", qDepth, jLevel);
      return true;
    }
  }

  //
  // Geo-query inter-parameter validation lives in ldParamsValidate now —
  // hoisted so /csourceRegistrations (and every other query route) shares
  // the identical checks and messages.
  //

  //
  // Build query filter from URL params
  //
  DbQueryFilter filter = {0};

  filter.idV       = corNgsild.idV;
  filter.idPattern = corNgsild.idPattern;
  filter.typeV     = corNgsild.typeV;
  filter.typeExpr  = corNgsild.typeExpr;
  filter.scopeExpr = corNgsild.scopeExpr;
  // The storage layer can only evaluate "layer 0" of the q — a § 4.9 linked
  // sub-query (q=rel{...}) needs the broker to follow the link (distops). Hand
  // the DB the linked-stripped tree so it returns an inclusive candidate set;
  // applyLinkedQPostFilter then resolves the linked layers against corNgsild.qExpr.
  filter.qExpr     = (corNgsild.qExpr != NULL && corNgsild.qExpr->linkedDepth > 0)
                     ? ldQStripLinked(corNgsild.qExpr, &corRest.kalloc)
                     : corNgsild.qExpr;
  filter.geoRel      = corNgsild.geoRel;
  filter.geometry    = corNgsild.geometry;
  filter.coordinates = corNgsild.coordinates;
  filter.geoproperty = corNgsild.geoproperty ? corNgsild.geoproperty : corLdExpand(corNgsild.contextP, "location", &corRest.kalloc, NULL, NULL);
  filter.limit     = (corNgsild.limit > 0) ? corNgsild.limit + 1 : 0;
  filter.offset   = corNgsild.offset;
  filter.count    = corNgsild.count;

  //
  // § 7.6.2.2 sort-by-distance: an orderBy "<geoprop>;dist-asc|dist-desc" term
  // ranks entities by distance from ?orderFrom to <geoprop>. It needs orderFrom
  // (a reference Point) and, for now, a Point orderGeometry (default). The plugin
  // computes each entity's geoDistance and orders geo-bearing entities first.
  //
  for (int i = 0; i < corNgsild.orderByCount; i++)
  {
    if (!corNgsild.orderByV[i].byDistance)
      continue;

    if (corNgsild.orderFrom == NULL)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid orderBy",
              "orderBy dist-asc/dist-desc requires the orderFrom parameter");
      return true;
    }
    if (corNgsild.orderGeometry != NULL && strcmp(corNgsild.orderGeometry, "Point") != 0)
    {
      ldError(501, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
              "sort by distance is only supported for a Point orderGeometry (got '%s')", corNgsild.orderGeometry);
      return true;
    }

    filter.distGeoproperty = (corNgsild.orderByV[i].pathSegN > 0) ? corNgsild.orderByV[i].pathSegV[0] : NULL;
    filter.distFrom        = corNgsild.orderFrom;
    filter.distDesc        = (corNgsild.orderByV[i].dir == LdOrderDesc);
    break;  // a single dist term drives the ranking
  }

  //
  // Determine split-entities mode: per-request param overrides global setting.
  // Split mode only activates when registrations actually match (checked below).
  //
  bool splitModeSetting = corNgsild.splitEntitiesSet ? corNgsild.splitEntitiesVal : ldSplitEntities;

  //
  // Query the local database (full filters for now — re-queried without
  // filters if split mode activates below)
  //
  KjNode* arrayP = NULL;
  int     r      = db.entityQuery((Tenant*) corNgsild.tenantP, &filter, &arrayP);

  if (r != DB_OK)
  {
    // Plugin filled in filter.err* on the way out. Surface to the
    // client as a ProblemDetails with the storage-layer wording.
    if (filter.errStatus != 0)
      ldError(filter.errStatus, filter.errType, filter.errTitle, "%s", filter.errDetail);
    else
      ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error querying entities");
    return true;
  }

  //
  // § 5.2.4 transient Entities: drop any whose entity-level expiresAt has
  // passed and queue them for deletion once the response is out. Done here,
  // before localQueryEmpty / pagination / the distributed merge, so an expired
  // Entity is invisible to every one of them. The DB query deliberately does
  // not filter on expiresAt — the rows have to reach RAM to be noticed.
  //
  dbExpiredEntityFilter((Tenant*) corNgsild.tenantP, arrayP);

  // § 6.4.7.2: whether the LOCAL query came back empty (before any
  // distributed merge / post-filter touches arrayP). An empty local page
  // with a positive offset is the offset-past-the-end case handled below.
  bool localQueryEmpty = (arrayP == NULL || arrayP->value.firstChildP == NULL);
  bool distForwarded   = false;

  //
  // Source-provenance map (§ 5.2.39) — built only when an EntityMap is
  // being created. Each entityId maps to the list of sources that
  // contributed to it: "@none" for local, a CSR regId otherwise. Without
  // this, paged follow-up via ?entityMap=<id> cannot route to the right
  // source for a given entity.
  //
  KjNode* srcMap = corNgsild.entityMapCreate ? kjObject(corRest.kjsonP, NULL) : NULL;
  srcMapStampLocalFrom(srcMap, arrayP);

  // Linked-maps tracker (§ 5.14.4.4) — KjObject keyed by CSR regId, value =
  // remote EntityMap id. Populated as each per-CSR forward returns its
  // own EntityMap; flushed into mapP after the local map is created.
  KjNode* linkedMapsTracker = corNgsild.entityMapCreate ? kjObject(corRest.kjsonP, NULL) : NULL;

  //
  // Distributed query: if registrations match and ?local=true is not set,
  // forward to matching CSRs and merge results.
  //
  // No-split mode: forward full query, merge + dedup
  // Split mode:    forward without filters, merge all attrs per entity,
  //                then apply filters post-assembly
  //
  if (corNgsild.local == false)
  {
    Tenant*           tP    = (Tenant*) corNgsild.tenantP;
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
      const char* ownAlias = ldCsourceAliasForTenant(tP->name, &corRest.kalloc);
      char**      pickWanted = computeWantedAttrs(&corRest.kalloc);

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
        // Prune the fan-out by the query's selectors: id/idPattern (never split
        // across sources) AND the entity type — for every mode except auxiliary
        // (gap-filler, queried broadly). Same as the temporal query path. This
        // is the spec-compliant behavior: a query must carry type/q/etc
        // (§ 5.7.2.4), so we honour the type and forward only to type-matching
        // sources. Pinned-id registrations that cannot provide any queried id
        // are excluded; regs constrained only by idPattern always match a query
        // ?idPattern= (regex-vs-regex is not computable — ETSI agreement).
        //
        // ACCEPTED LIMITATION (pending an ETSI clarification KZ is driving): a
        // MULTI-TYPE entity whose attributes are split across sources that each
        // register only a SUBSET of its types loses the other-typed source's
        // attributes (a {type:A} source holding an attr of an A+B entity is not
        // reached by a ?type=B query). The only spec-clean completion is a
        // second id-list query to the type-complement sources — deferred until
        // the spec direction is settled (it may instead become type-scoped
        // views, making completion unnecessary). Earlier this skipped the type
        // prune entirely in split mode to avoid the loss, but that over-forwarded
        // every typed query to all tenant registrations (waste + leaking the
        // query/filters to sources that don't serve the type).
        modeMatchN[m] = ldRegCacheMatchForQuery((LdRegCache*) tP->regCacheP,
                                                corNgsild.idV, corNgsild.idPattern,
                                                (modes[m] == LdRegModeAuxiliary) ? NULL : corNgsild.typeV,
                                                modes[m], &modeMatchV[m]);
        totalMatch += modeMatchN[m];
      }

      // Registrations contribute → the result is no longer purely local,
      // so the offset-past-end clamp below does not apply (it has no single
      // result-set size N to clamp against).
      distForwarded = (totalMatch > 0);

      if (totalMatch > 0 && splitModeSetting)
      {
        splitMode = true;
        DbQueryFilter splitFilter = {0};
        splitFilter.idV       = filter.idV;
        splitFilter.idPattern = filter.idPattern;
        splitFilter.limit     = 1000000;
        arrayP = NULL;
        db.entityQuery((Tenant*) corNgsild.tenantP, &splitFilter, &arrayP);
        srcMapStampLocalFrom(srcMap, arrayP);
      }

      // baseQs is per-CSR (alias-bearing params are compacted via ldDistOpForwardContext)
      // so it gets computed inside the loop now.

      LdDistOpBatchItem*   items   = (LdDistOpBatchItem*)   kaAlloc(&corRest.kalloc, totalMatch * sizeof(LdDistOpBatchItem));
      memset(items, 0, totalMatch * sizeof(LdDistOpBatchItem));

      // § 9.2 operations: a CSR may support queryEntity (GET /entities),
      // queryBatch (POST /entityOperations/query), both, or neither.
      // Prefer mirroring the incoming form; fall back to the other; skip
      // CSRs that support neither query op.
      bool incomingBatch = (corRest.in.verb == CorVerbPost);
      LdDistOpBatchResult* results = (LdDistOpBatchResult*) kaAlloc(&corRest.kalloc, totalMatch * sizeof(LdDistOpBatchResult));
      int                  itemCount = 0;
      memset(results, 0, totalMatch * sizeof(LdDistOpBatchResult));

      for (int m = 0; m < 4; m++)
      {
        for (int i = 0; i < modeMatchN[m]; i++)
        {
          LdRegCacheItem* csr   = modeMatchV[m][i];
          const char*     regId = (csr->regId != NULL) ? csr->regId : "<no id>";

          if (csr->endpoint == NULL)
          {
            KT_T(LdTRegMatch, "%s: matched, but NOT forwarded to: the registration has no endpoint", regId);
            continue;
          }
          if (ldDistOpCsrWouldLoop(csr, ownAlias)) continue;   // traces its own reason

          bool csrQE = ldRegOpSupported(csr, LdOpQueryEntities);
          bool csrQB = ldRegOpSupported(csr, LdOpBatchQuery);
          if (!csrQE && !csrQB)
          {
            KT_T(LdTRegMatch, "%s: matched, but NOT forwarded to: 'operations' covers neither queryEntity nor batch query", regId);
            continue;
          }
          bool postForward = incomingBatch ? csrQB : !csrQE;

          //
          // § 5.2.23 csf — the Context Source Filter selects which Context
          // Source Registrations may serve the query. It is matched against
          // the registration itself (its user-Properties), never against the
          // Entities; a registration we cannot inspect cannot satisfy it.
          //
          if (corNgsild.csfExpr != NULL)
          {
            if ((csr->regTree == NULL) || !ldEntityMatchQ(csr->regTree, corNgsild.csfExpr))
            {
              KT_T(LdTRegMatch, "%s: matched, but NOT forwarded to: the registration does not match csf '%s'", regId, corNgsild.csf);
              continue;
            }
          }

          if (corNgsild.geoRel != NULL && ((LdRegCache*) tP->regCacheP)->csrGeoMatchFunc != NULL)
          {
            const char* prop = corNgsild.geoproperty;
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
            if (!((LdRegCache*) tP->regCacheP)->csrGeoMatchFunc(csrGeoP, corNgsild.geoRel,
                                                                 corNgsild.geometry, corNgsild.coordinates))
              continue;
          }

          const char* fullQs;
          if (splitMode)
          {
            char** csrExports = csrUnionExports(csr, &corRest.kalloc);
            bool   skip       = false;
            const char* pickParam = intersectAndPick(csrExports, pickWanted, &corRest.kalloc, &skip, ldDistOpForwardContext(csr));
            if (skip) continue;

            const char* splitBase = buildSplitForwardQueryString(ldDistOpForwardContext(csr));
            const char* idParam   = csrPinnedIdsParam(csr, &corRest.kalloc);
            int   bLen = strlen(splitBase), pLen = strlen(pickParam), iLen = strlen(idParam);
            char* combined = (char*) kaAlloc(&corRest.kalloc, bLen + pLen + iLen + 1);
            strcpy(combined, splitBase);
            if (iLen > 0) strcpy(combined + bLen, idParam);
            if (pLen > 0) strcpy(combined + bLen + iLen, pickParam);
            fullQs = combined;
          }
          else
          {
            const char* baseQs = buildQueryString(ldDistOpForwardContext(csr));
            fullQs = baseQs;
            bool csrSkipped = false;

            for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
            {
              if (corNgsild.typeV != NULL)
              {
                bool typeMatch = false;
                for (LdRegEntityInfo* eiP = riP->entityInfoV; eiP != NULL; eiP = eiP->next)
                {
                  if (eiP->type == NULL) { typeMatch = true; break; }
                  for (int t = 0; corNgsild.typeV[t] != NULL; t++)
                    if (strcmp(eiP->type, corNgsild.typeV[t]) == 0) { typeMatch = true; break; }
                  if (typeMatch) break;
                }
                if (!typeMatch) continue;
              }

              bool        skip      = false;
              const char* pickParam = intersectAndPick(riP->attributeNamesV,
                                                        pickWanted, &corRest.kalloc, &skip, ldDistOpForwardContext(csr));
              if (skip) { csrSkipped = true; break; }

              const char* idParam = csrPinnedIdsParam(csr, &corRest.kalloc);
              int   bLen = strlen(baseQs), pLen = strlen(pickParam), iLen = strlen(idParam);
              char* combined = (char*) kaAlloc(&corRest.kalloc, bLen + pLen + iLen + 1);
              strcpy(combined, baseQs);
              if (iLen > 0) strcpy(combined + bLen, idParam);
              strcpy(combined + bLen + iLen, pickParam);
              fullQs = combined;
              break;
            }

            if (csrSkipped) continue;
          }

          if (postForward && !corNgsild.entityMapCreate)
          {
            // queryBatch form: POST /entityOperations/query with a § 5.2.23
            // Query body carrying the same selectors + projection as the
            // GET form's query string.
            // sysAttrs=true for the same reason the GET form carries it (see
            // buildSplitForwardQueryString): without modifiedAt on the
            // forwarded instances, § 4.5.5.3 rule 3 has nothing to compare.
            // It rides the URL, not the Query body - buildQueryBodyFromQs
            // maps only the § 5.2.23 body members and drops everything else.
            const char* path    = (splitMode || corNgsild.sysAttrs)
                                    ? "/ngsi-ld/v1/entityOperations/query?sysAttrs=true"
                                    : "/ngsi-ld/v1/entityOperations/query";
            int   baseLen = strlen(csr->endpoint);
            char* url     = (char*) kaAlloc(&corRest.kalloc, baseLen + strlen(path) + 1);
            strcpy(url, csr->endpoint);
            strcpy(url + baseLen, path);

            const char* body = buildQueryBodyFromQs(fullQs, &corRest.kalloc);

            KT_T(KtDistOpRequest, "forward: POST %s body=%s", url, body);
            items[itemCount].csr     = csr;
            items[itemCount].url     = url;
            items[itemCount].body    = body;
            items[itemCount].bodyLen = strlen(body);
            items[itemCount].hasVerb = true;
            items[itemCount].verb    = CorVerbPost;
            itemCount++;
            continue;
          }

          const char* path    = corNgsild.entityMapCreate ? "/ngsi-ld/v1/entityMaps?"
                                                         : "/ngsi-ld/v1/entities?";
          int   baseLen = strlen(csr->endpoint);
          int   pathLen = strlen(path);
          int   qsLen   = strlen(fullQs);
          char* url     = (char*) kaAlloc(&corRest.kalloc, baseLen + pathLen + qsLen + 1);
          strcpy(url, csr->endpoint);
          strcpy(url + baseLen, path);
          strcpy(url + baseLen + pathLen, fullQs);

          KT_T(KtDistOpRequest, "forward: GET %s", url);
          items[itemCount].csr     = csr;
          items[itemCount].url     = url;
          items[itemCount].body    = NULL;
          items[itemCount].bodyLen = 0;
          itemCount++;
        }
      }

      if (itemCount > 0)
      {
        ldDistOpSendMulti(items, itemCount, CorVerbGet, ownAlias, results);

        for (int i = 0; i < itemCount; i++)
        {
          LdRegCacheItem* csr    = items[i].csr;
          int             code   = results[i].statusCode;

          // responseBody is tokenized in place by the reception-time parse, so
          // the raw buffer is no longer printable — render the parsed tree back
          // to a string for the trace instead.
          const char* renderedBody = "(none)";
          if (results[i].responseTree != NULL)
          {
            int   rsz  = kjFastRenderSize(results[i].responseTree) + 1;
            char* rbuf = (char*) kaAlloc(&corRest.kalloc, rsz);
            kjFastRender(results[i].responseTree, rbuf);
            renderedBody = rbuf;
          }
          KT_T(KtDistOpRequest, "forward response: status=%d, bodyLen=%d, error=%s, body=%s",
               code, results[i].responseBodyLen,
               results[i].errorDetail != NULL ? results[i].errorDetail : "(none)",
               renderedBody);

          if (code < 200 || code >= 300) continue;
          if (results[i].responseBody == NULL || results[i].responseBodyLen == 0) continue;

          KjNode* remoteArray;

          if (corNgsild.entityMapCreate)
          {
            // § 5.14.4.4: response is a single EntityMap object. Pull out
            // remote map id + synthesise an array of { "id": <entityId> }
            // entries so the dedup loop below stays format-agnostic.
            KjNode* mapTreeP = results[i].responseTree;
            if (mapTreeP == NULL || mapTreeP->type != KjObject) continue;

            KjNode* idP = kjLookup(mapTreeP, "id");
            if (linkedMapsTracker != NULL && idP != NULL && idP->type == KjString && csr->regId != NULL)
              kjChildAdd(linkedMapsTracker, kjString(corRest.kjsonP, csr->regId, idP->value.s));

            KjNode* emObj = kjLookup(mapTreeP, "entityMap");
            if (emObj == NULL || emObj->type != KjObject) continue;

            remoteArray = kjArray(corRest.kjsonP, NULL);
            for (KjNode* entryP = emObj->value.firstChildP; entryP != NULL; entryP = entryP->next)
            {
              if (entryP->name == NULL) continue;
              KjNode* synth = kjObject(corRest.kjsonP, NULL);
              kjChildAdd(synth, kjString(corRest.kjsonP, "id", entryP->name));
              kjChildAdd(remoteArray, synth);
            }
          }
          else
          {
            remoteArray = results[i].responseTree;
            // @context is stripped per-entity AFTER expansion below (a json
            // response carries it in the Link header, an ld+json response
            // embeds it per element — both feed the per-entity expand).

            // § 6.3.16 / JSON-LD compaction: a one-element array may be
            // unwrapped to its member alone. Accept a bare entity object
            // and re-wrap it so the rest of the merge loop stays array-
            // typed.
            if (remoteArray != NULL && remoteArray->type == KjObject)
            {
              KjNode* wrap = kjArray(corRest.kjsonP, NULL);
              kjChildAdd(wrap, remoteArray);
              remoteArray = wrap;
            }
          }

          if (remoteArray == NULL || remoteArray->type != KjArray) continue;

          // Expand each forwarded entity via the context that travels WITH the
          // response — the URL in its json-ld#context Link header, else core.
          // corLdExpandTree additionally applies any embedded @context (ld+json)
          // on top. NOT corNgsild.contextP: the CP speaks its own vocabulary.
          CorLdContext* respCtxP = (results[i].responseContextUrl != NULL)
                                    ? corLdContextFromUrl(results[i].responseContextUrl, &corRest.kalloc)
                                    : NULL;
          if (respCtxP == NULL)
            respCtxP = corLdCoreContext();

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
            corLdExpandTree(remoteEntity, respCtxP, &corRest.kalloc);
            ldStripAtContext(remoteEntity);
            apiAttrToStorageWrap(remoteEntity);

            // § 4.5.5.2 — this version's entity-level expiresAt cascades onto
            // each of its Attributes (shortening any attr-level value further
            // in the future) BEFORE the entity-level values are reconciled
            // across versions below. Same as the retrieve-one path.
            ldExpiresAtPropagate(remoteEntity, corRest.kjsonP);

            // Runtime exclusive-priority: an attribute exclusively claimed by another
            // registration is authoritative from that (exclusive) source alone. Discard any
            // colliding copy arriving from THIS non-exclusive source — otherwise the timestamp
            // tiebreaker in the merge below could let a newer inclusive value overwrite the
            // exclusive one (§ 4.5.5 / mode semantics). Sources are fanned out exclusive-first,
            // so the exclusive copy is already in place by the time inclusive results merge.
            if (csr->mode != LdRegModeExclusive)
            {
              LdRegCache* excRc  = (LdRegCache*) ((Tenant*) corNgsild.tenantP)->regCacheP;
              KjNode*     etP    = kjLookup(remoteEntity, "type");
              char*       etV[2] = { (etP != NULL && etP->type == KjString) ? etP->value.s : NULL, NULL };
              for (KjNode* aP = remoteEntity->value.firstChildP; aP != NULL; )
              {
                KjNode* nextAP = aP->next;
                if (aP->name != NULL && aP->name[0] != '@' &&
                    strcmp(aP->name, "id") != 0 && strcmp(aP->name, "type") != 0 &&
                    ldRegCacheAttrExclusivelyClaimed(excRc, remoteIdP->value.s, etV[0] != NULL ? etV : NULL, aP->name, corRest.requestStartTime))
                  kjChildRemove(remoteEntity, aP);
                aP = nextAP;
              }
            }

            if (existingP == NULL)
            {
              kjChildAdd(arrayP, remoteEntity);
              srcMapAdd(srcMap, remoteIdP->value.s, csr->regId);
            }
            else if (splitMode)
            {
              srcMapAdd(srcMap, remoteIdP->value.s, csr->regId);

              //
              // § 4.5.5.3 - one more version of this Entity. Everything the
              // merge consists of (entity-level expiresAt, § 5.2.7 Scopes,
              // per-(attr, dsKey) conflict resolution) lives in one shared
              // function, so the query path and the retrieve-one path cannot
              // drift apart again. clone=false: remoteEntity is a per-request
              // tree, its instances move rather than being copied.
              //
              ldDistMergeSourceInto(existingP, remoteEntity, corRest.requestStartTime, corRest.kjsonP, false);
            }

            remoteEntity = nextRemote;
          }
        }
      }

      for (int m = 0; m < 4; m++)
        if (modeMatchV[m] != NULL) free(modeMatchV[m]);
      (void) matchV;
      (void) matchN;

      // § 6.3.5 — a forwarded source that behaved abnormally (an HTTP error
      // status such as 403, a timeout/no-response, or an invalid payload) is
      // tolerated (its data is simply absent from the merge), but the abnormal
      // behaviour must be signalled with an NGSILD-Warning (299/199/111).
      char* warn = ldDistOpWarnings(items, results, itemCount);
      if (warn != NULL)
        corRestOutHeaderAdd("NGSILD-Warning", warn);
    }

    // Split mode post-assembly filters (§ 5.7.2.4).
    if (splitMode)
      applyResultFilters(arrayP);
  }

  //
  // Sort by orderBy before pagination (§ 4.23)
  //
  if (corNgsild.orderByV != NULL && corNgsild.orderByCount > 0)
    ldOrderSort(arrayP, corNgsild.orderByV, corNgsild.orderByCount, corNgsild.collation);

  //
  // Entity map: if entityMap=true, freeze the sorted entity IDs into a map
  // for consistent pagination. The map is stored per-tenant and its location
  // is returned via the NGSILD-EntityMap response header.
  //
  if (corNgsild.entityMapCreate)
  {
    Tenant* tP = (Tenant*) corNgsild.tenantP;

    if (tP != NULL && tP->entityMapStoreP != NULL)
    {
      // Purge expired maps first
      ldEntityMapPurgeExpired((LdEntityMapStore*) tP->entityMapStoreP);

      // Default lifetime: 5 minutes
      LdEntityMap* mapP = ldEntityMapCreate((LdEntityMapStore*) tP->entityMapStoreP,
                                             5ULL * 60 * 1000000000ULL, tP);

      // Bind the query's filter params to the map (§ 9 same-parameters): a
      // later paginated reuse may re-send these with the same value or omit
      // them, but not modify or introduce one.
      ldEntityMapSetFilters(mapP, corNgsild.type, corNgsild.q, corNgsild.scopeQ,
                            corNgsild.georel, corNgsild.geometry, corNgsild.coordinates,
                            corNgsild.geoproperty);

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
          const char** srcV = (const char**) kaAlloc(&corRest.kalloc, n * sizeof(char*));
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
      char* mapUrl = (char*) kaAlloc(&corRest.kalloc, 128);
      snprintf(mapUrl, 128, "/ngsi-ld/v1/entityMaps/%s", mapP->mapId);

      corRestOutHeaderAdd("NGSILD-EntityMap", mapUrl);

      //
      // GET / POST /entityMaps (§ 6.34.3): return the EntityMap itself
      // (201 Created) instead of the matching entities array. rawResponse
      // lets ldEntityMapToTree's JSON flow unchanged through renderHook.
      //
      if (corNgsild.entityMapOnly)
      {
        corRest.out.responseTree   = ldEntityMapToTree(mapP);
        corRest.out.httpStatusCode = 201;
        corNgsild.rawResponse      = true;
        return true;
      }
    }
  }

  //
  // § 6.4.7.2: "If offset is set to a value larger than the result set, the
  // offset should be assumed to be equal to the size of the result set, i.e.
  // only the last element of the result set is to be returned if there are any
  // results." So an offset past the end returns the LAST entity, not an empty
  // page. Only the local (non-distributed, non-EntityMap) path has a single
  // well-defined result-set size N to clamp against; the distributed merge has
  // no single N (the EntityMap-follow path clamps separately, by entryCount).
  // Cost is paid only in this rare overshoot case: one count query (skipped if
  // ?count already computed N), then a one-element fetch at offset N-1.
  //
  if (localQueryEmpty && !distForwarded && !corNgsild.entityMapCreate && corNgsild.offset > 0)
  {
    int64_t n;

    if (corNgsild.count)
      n = filter.totalCount;   // the query above already counted the result set
    else
    {
      DbQueryFilter countFilter = filter;
      countFilter.count = true;
      countFilter.limit = 0;   // count-only, no entities materialized
      KjNode* dummyP    = NULL;
      db.entityQuery((Tenant*) corNgsild.tenantP, &countFilter, &dummyP);
      n = countFilter.totalCount;
    }

    if (n > 0)
    {
      DbQueryFilter lastFilter = filter;
      lastFilter.offset = (int) (n - 1);
      lastFilter.limit  = 1;   // exactly the last element
      lastFilter.count  = false;
      arrayP = NULL;
      db.entityQuery((Tenant*) corNgsild.tenantP, &lastFilter, &arrayP);
    }
  }

  //
  // Add NGSILD-Results-Count header if count was requested
  //
  if (corNgsild.count)
  {
    char* countStr = (char*) kaAlloc(&corRest.kalloc, 32);
    snprintf(countStr, 32, "%ld", (long) filter.totalCount);

    corRestOutHeaderAdd("NGSILD-Results-Count", countStr);
  }

  //
  // Pagination: trim to limit and add Link header with next/prev.
  // § 7.4.2.2: the prev/next pointers describe iterating the pages of a result
  // set. A page that is EMPTY *and* has nothing more pending is no pagination
  // iteration, so emit neither (the prev pointer is mandated only "for all
  // pagination iterations excepting the first one"). When more pages remain
  // (hasMore) the next pointer is kept even on an empty page so the client can
  // still advance. Without this guard a query past the end of an empty result
  // set emitted a spurious rel="prev" to offset=0.
  //
  bool hasMore = ldPaginationTrim(arrayP, corNgsild.limit);
  if ((arrayP != NULL && arrayP->value.firstChildP != NULL) || hasMore)
    ldPaginationLinkHeader(hasMore);

  //
  // Apply pick/omit attribute projection (or the legacy attrs alias)
  //
  if (corNgsild.pickV != NULL || corNgsild.omitV != NULL)
  {
    for (KjNode* entityP = arrayP->value.firstChildP; entityP != NULL; entityP = entityP->next)
      ldPickOmit(entityP, corNgsild.pickV, corNgsild.omitV);

    // Drop entities reduced to empty by pick — see the earlier
    // identical block; spec is silent, ETSI plenary chose to drop.
    if (corNgsild.pickV != NULL)
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
  else if (corNgsild.attrsV != NULL)
  {
    // attrs = selection + projection: drop entities with none of the
    // listed attributes (see the identical block in the local path).
    KjNode* ep = arrayP->value.firstChildP;
    while (ep != NULL)
    {
      KjNode* next = ep->next;
      ldAttrsFilter(ep, corNgsild.attrsV);

      bool hasAttr = false;
      for (KjNode* cP = ep->value.firstChildP; cP != NULL; cP = cP->next)
      {
        if (cP->name != NULL && !ldIsEntityKeyword(cP->name)) { hasAttr = true; break; }
      }
      if (!hasAttr)
        kjChildRemove(arrayP, ep);
      ep = next;
    }
  }

  // § 4.9 LinkedEntityRelation — post-filter when q contains a sub-q.
  applyLinkedQPostFilter(arrayP);

  // § 4.5.23 — linked-entity expansion of each result.
  if (corNgsild.join != NULL)
  {
    int level = (corNgsild.joinLevel > 0) ? corNgsild.joinLevel : 1;
    Tenant* tP = (Tenant*) corNgsild.tenantP;
    if      (strcmp(corNgsild.join, "flat")   == 0) ldLinkedEntitiesExpandArrayFlat(arrayP, level, tP);
    else if (strcmp(corNgsild.join, "inline") == 0) ldLinkedEntitiesExpandArrayInline(arrayP, level, tP);
  }

  corRest.out.responseTree = arrayP;
  return true;
}

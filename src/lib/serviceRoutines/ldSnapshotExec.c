//
// FILE            ldSnapshotExec.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Snapshot query execution — see header.
//
#include <stdbool.h>                                     // bool
#include <stdio.h>                                       // snprintf
#include <stdlib.h>                                      // free
#include <string.h>                                      // strcmp, strlen, strcpy, memcpy

#include "kalloc/kaAlloc.h"                              // kaAlloc
#include "kjson/KjNode.h"                                // KjNode
#include "kjson/kjLookup.h"                              // kjLookup
#include "kjson/kjBuilder.h"                             // kjArray, kjObject, kjString, kjChildAdd
#include "kjson/kjParse.h"                               // kjParse

#include "swRest/SwRestState.h"                          // swRest
#include "swRest/swRestClient.h"                         // SwRestClientRequest, swRestClientSend

#include "swJsonld/swldExpand.h"                         // swldExpand, swldAlreadyExpanded
#include "swJsonld/swldExpandTree.h"                     // swldExpandTree
#include "swNgsild/swNgsild.h"                           // swNgsild
#include "swNgsild/ldInit.h"                             // ldSplitEntities (extern)
#include "swNgsild/ldQParse.h"                           // ldQParse
#include "swNgsild/LdScopeExpr.h"                        // ldScopeExprParse
#include "swNgsild/LdRegCache.h"                         // LdRegCache, LdRegCacheItem, LdRegMode
#include "swNgsild/ldRegCache.h"                         // ldRegCacheMatchForRetrieve
#include "swNgsild/ldStripAtContext.h"                   // ldStripAtContext
#include "swNgsild/ldApiEntityToDbModel.h"               // ldApiEntityToDbModel
#include "swNgsild/ldDistMerge.h"                        // ldDistInstanceShouldReplace, ldDistInstanceIsExpired, ldDistExpiresAtReconcile
#include "swNgsild/ldEntityMatch.h"                      // ldEntityMatchType, ldEntityMatchQ, ldEntityMatchScope
#include "swNgsild/LdSnapshotCache.h"                    // LdSnapshotCache*

#include "kjson/kjClone.h"                               // kjClone
#include "kjson/kjFree.h"                                // kjFree
#include "kjson/kjChildReplace.h"                        // kjChildReplace

#include "db/DbDriver.h"                                 // db, DB_OK, DB_ALREADY_EXISTS
#include "db/DbQueryFilter.h"                            // DbQueryFilter
#include "db/Tenant.h"                                   // Tenant

#include "serviceRoutines/ldSnapshotExec.h"              // Own interface



//
// expandedTypeOrSelf - return entitiesP[i].type expanded against the request context.
//
static const char* expandedTypeOrSelf(const char* shortName)
{
  if (shortName == NULL)
    return NULL;
  if (swldAlreadyExpanded(shortName))
    return shortName;
  return swldExpand(swNgsild.contextP, shortName, &swRest.kalloc, NULL, NULL);
}



//
// entitySelectorsToFilter - extract id / idPattern / type from a Query's
// entities[] array into the DbQueryFilter.
//
// Spec § 5.2.33: id may be a single string OR an array of strings;
// idPattern is a single regex string. type is required on every selector
// (the Query type validates this elsewhere). When multiple selectors are
// provided, the broker unions the (id, idPattern, type) sets — there is
// no real OR-of-AND machinery here, since the DB layer accepts a single
// idV / idPattern / typeV triple.
//
// For Phase 2a we accept multi-selector with the simplification that the
// id and idPattern from each selector are unioned into one flat list,
// and the type values are unioned into typeV. This matches the typical
// snapshot use-case (one selector or several with disjoint types).
//
static void entitySelectorsToFilter(KjNode* entitiesP, DbQueryFilter* filterP)
{
  if (entitiesP == NULL || entitiesP->type != KjArray)
    return;

  // Worst-case capacity for id / type vectors.
  int idCap = 0, typeCap = 0;
  for (KjNode* selP = entitiesP->value.firstChildP; selP != NULL; selP = selP->next)
  {
    if (selP->type != KjObject) continue;

    KjNode* idP = kjLookup(selP, "id");
    if (idP != NULL)
    {
      if      (idP->type == KjString) idCap++;
      else if (idP->type == KjArray)
        for (KjNode* p = idP->value.firstChildP; p != NULL; p = p->next) idCap++;
    }
    if (kjLookup(selP, "type") != NULL) typeCap++;
  }

  char** idV   = (idCap   > 0) ? (char**) kaAlloc(&swRest.kalloc, (idCap   + 1) * sizeof(char*)) : NULL;
  char** typeV = (typeCap > 0) ? (char**) kaAlloc(&swRest.kalloc, (typeCap + 1) * sizeof(char*)) : NULL;
  int    nId = 0, nType = 0;
  const char* idPattern = NULL;

  for (KjNode* selP = entitiesP->value.firstChildP; selP != NULL; selP = selP->next)
  {
    if (selP->type != KjObject) continue;

    KjNode* idP = kjLookup(selP, "id");
    if (idP != NULL)
    {
      if (idP->type == KjString)
        idV[nId++] = idP->value.s;
      else if (idP->type == KjArray)
        for (KjNode* p = idP->value.firstChildP; p != NULL; p = p->next)
          if (p->type == KjString) idV[nId++] = p->value.s;
    }

    KjNode* idPatP = kjLookup(selP, "idPattern");
    if (idPatP != NULL && idPatP->type == KjString && idPattern == NULL)
      idPattern = idPatP->value.s;

    KjNode* typeP = kjLookup(selP, "type");
    if (typeP != NULL && typeP->type == KjString)
    {
      const char* expanded = expandedTypeOrSelf(typeP->value.s);
      if (expanded != NULL)
        typeV[nType++] = (char*) expanded;
    }
  }

  if (idV   != NULL) idV[nId]     = NULL;
  if (typeV != NULL) typeV[nType] = NULL;

  filterP->idV       = (nId   > 0) ? idV   : NULL;
  filterP->typeV     = (nType > 0) ? typeV : NULL;
  filterP->idPattern = (char*) idPattern;
}



//
// queryToFilter - translate one § 5.2.23 Query JSON object into a
// DbQueryFilter.
//
// Phase 2a supports: entities[] (id, idPattern, type), q, scopeQ.
// Geo-query and temporalQ are deferred — temporalQ is only valid for
// snapshotTemporalQueries which Phase 2a doesn't execute, and geoQ
// requires JSON-array → string conversion of `coordinates` plus
// alignment with the request-time geoMatchFunc plumbing; it can land
// in 2c without a wire-format break.
//
static void queryToFilter(KjNode* queryP, DbQueryFilter* filterP)
{
  filterP->limit = 0;        // unbounded
  filterP->offset = 0;
  filterP->count = false;

  KjNode* entitiesP = kjLookup(queryP, "entities");
  entitySelectorsToFilter(entitiesP, filterP);

  KjNode* qP = kjLookup(queryP, "q");
  if (qP != NULL && qP->type == KjString)
    filterP->qExpr = ldQParse(qP->value.s, &swRest.kalloc);

  KjNode* scopeQP = kjLookup(queryP, "scopeQ");
  if (scopeQP != NULL && scopeQP->type == KjString)
    filterP->scopeExpr = ldScopeExprParse(scopeQP->value.s, &swRest.kalloc);
}



//
// buildQueryStringFromSnapshotQuery - render a § 5.2.23 Query as the
// URL parameters expected by GET /entities. The current snapshot DB
// filter is reused for typeV / qExpr / scopeExpr; we serialize from
// the JSON Query directly because the captured filter on the receiver
// side runs against the live tenant and should look like a normal
// query, not the broker's internal struct.
//
// Phase 141 first cut: id, idPattern, type, q, scopeQ. geoQ is omitted
// pending JSON-array → URL string serialisation; affects only geo
// snapshots and a follow-up will fold it in.
//
static const char* buildQueryStringFromSnapshotQuery(KjNode* queryP, KAlloc* kaP)
{
  char* qs = (char*) kaAlloc(kaP, 4096);
  int   pos = 0;

  #define APPEND_KV(k, v) do {                                                  \
      int kl = strlen(k); int vl = strlen(v);                                   \
      if (pos > 0 && pos < 4095) qs[pos++] = '&';                               \
      memcpy(qs + pos, k, kl); pos += kl;                                       \
      qs[pos++] = '=';                                                          \
      memcpy(qs + pos, v, vl); pos += vl;                                       \
    } while (0)

  // entities[]: id (single string OR array), idPattern, type. URL params
  // accept a single value per key, so a multi-selector Query collapses
  // to "type=A,B" / "id=u1,u2". Phase 141 keeps it minimal — first
  // selector wins for id/idPattern.
  KjNode* entitiesP = kjLookup(queryP, "entities");
  if (entitiesP != NULL && entitiesP->type == KjArray)
  {
    char  typeBuf[1024]; int  typeLen = 0;  typeBuf[0] = 0;
    char  idBuf  [4096]; int  idLen   = 0;  idBuf  [0] = 0;
    const char* idPattern = NULL;

    for (KjNode* selP = entitiesP->value.firstChildP; selP != NULL; selP = selP->next)
    {
      if (selP->type != KjObject) continue;

      KjNode* tP = kjLookup(selP, "type");
      if (tP != NULL && tP->type == KjString)
      {
        int tl = strlen(tP->value.s);
        if (typeLen + tl + 2 < (int) sizeof(typeBuf))
        {
          if (typeLen > 0) typeBuf[typeLen++] = ',';
          memcpy(typeBuf + typeLen, tP->value.s, tl); typeLen += tl;
          typeBuf[typeLen] = 0;
        }
      }

      KjNode* iP = kjLookup(selP, "id");
      if (iP != NULL)
      {
        if (iP->type == KjString)
        {
          int il = strlen(iP->value.s);
          if (idLen + il + 2 < (int) sizeof(idBuf))
          {
            if (idLen > 0) idBuf[idLen++] = ',';
            memcpy(idBuf + idLen, iP->value.s, il); idLen += il;
            idBuf[idLen] = 0;
          }
        }
        else if (iP->type == KjArray)
        {
          for (KjNode* p = iP->value.firstChildP; p != NULL; p = p->next)
          {
            if (p->type != KjString) continue;
            int il = strlen(p->value.s);
            if (idLen + il + 2 < (int) sizeof(idBuf))
            {
              if (idLen > 0) idBuf[idLen++] = ',';
              memcpy(idBuf + idLen, p->value.s, il); idLen += il;
              idBuf[idLen] = 0;
            }
          }
        }
      }

      KjNode* ipP = kjLookup(selP, "idPattern");
      if (ipP != NULL && ipP->type == KjString && idPattern == NULL)
        idPattern = ipP->value.s;
    }

    if (typeLen  > 0) APPEND_KV("type",      typeBuf);
    if (idLen    > 0) APPEND_KV("id",        idBuf);
    if (idPattern) APPEND_KV("idPattern", idPattern);
  }

  KjNode* qP = kjLookup(queryP, "q");
  if (qP != NULL && qP->type == KjString)
    APPEND_KV("q", qP->value.s);

  KjNode* scopeQP = kjLookup(queryP, "scopeQ");
  if (scopeQP != NULL && scopeQP->type == KjString)
    APPEND_KV("scopeQ", scopeQP->value.s);

  // No local=true here: snapshot capture wants full transitive federation
  // (CB → CP1 → CP1's own CSRs → ...), bounded by § 5.12 loop detection.
  // The snapshotQuery's own type/q/scopeQ already satisfy the receiver's
  // too-wide-query check.

  qs[pos] = 0;
  return qs;

  #undef APPEND_KV
}



//
// buildSplitForwardQs - URL params to forward in split mode.
//
// Split mode can't safely forward q/scopeQ/geoQ (they may reference
// sharded attributes the receiver doesn't fully hold). It CAN forward
// type/id/idPattern — those are guaranteed on every fragment (§ 4.5.5,
// § 5.2.6) and serve as the too-wide-query buster at the receiver.
// No local=true → transitive fanout works, bounded by loop detection.
//
// Last-resort fallback: if the snapshotQuery carries none of
// type/id/idPattern, fall back to local=true to satisfy the receiver's
// minimum-filter check (this disables transitive fanout for that hop).
//
static const char* buildSplitForwardQs(KjNode* queryP, KAlloc* kaP)
{
  char* qs = (char*) kaAlloc(kaP, 4096);
  int   pos = 0;

  #define APPEND_KV(k, v) do {                                                  \
      int kl = strlen(k); int vl = strlen(v);                                   \
      if (pos > 0 && pos < 4095) qs[pos++] = '&';                               \
      memcpy(qs + pos, k, kl); pos += kl;                                       \
      qs[pos++] = '=';                                                          \
      memcpy(qs + pos, v, vl); pos += vl;                                       \
    } while (0)

  KjNode* entitiesP = kjLookup(queryP, "entities");
  if (entitiesP != NULL && entitiesP->type == KjArray)
  {
    char  typeBuf[1024]; int typeLen = 0; typeBuf[0] = 0;
    char  idBuf  [4096]; int idLen   = 0; idBuf  [0] = 0;
    const char* idPattern = NULL;

    for (KjNode* selP = entitiesP->value.firstChildP; selP != NULL; selP = selP->next)
    {
      if (selP->type != KjObject) continue;

      KjNode* tP = kjLookup(selP, "type");
      if (tP != NULL && tP->type == KjString)
      {
        int tl = strlen(tP->value.s);
        if (typeLen + tl + 2 < (int) sizeof(typeBuf))
        {
          if (typeLen > 0) typeBuf[typeLen++] = ',';
          memcpy(typeBuf + typeLen, tP->value.s, tl); typeLen += tl;
          typeBuf[typeLen] = 0;
        }
      }

      KjNode* iP = kjLookup(selP, "id");
      if (iP != NULL)
      {
        if (iP->type == KjString)
        {
          int il = strlen(iP->value.s);
          if (idLen + il + 2 < (int) sizeof(idBuf))
          {
            if (idLen > 0) idBuf[idLen++] = ',';
            memcpy(idBuf + idLen, iP->value.s, il); idLen += il;
            idBuf[idLen] = 0;
          }
        }
        else if (iP->type == KjArray)
        {
          for (KjNode* p = iP->value.firstChildP; p != NULL; p = p->next)
          {
            if (p->type != KjString) continue;
            int il = strlen(p->value.s);
            if (idLen + il + 2 < (int) sizeof(idBuf))
            {
              if (idLen > 0) idBuf[idLen++] = ',';
              memcpy(idBuf + idLen, p->value.s, il); idLen += il;
              idBuf[idLen] = 0;
            }
          }
        }
      }

      KjNode* ipP = kjLookup(selP, "idPattern");
      if (ipP != NULL && ipP->type == KjString && idPattern == NULL)
        idPattern = ipP->value.s;
    }

    if (typeLen > 0) APPEND_KV("type",      typeBuf);
    if (idLen   > 0) APPEND_KV("id",        idBuf);
    if (idPattern)   APPEND_KV("idPattern", idPattern);
  }

  // Last-resort: nothing to forward → satisfy the too-wide-query check
  // with local=true (suppresses fanout for this hop only).
  if (pos == 0)
    APPEND_KV("local", "true");

  qs[pos] = 0;
  return qs;

  #undef APPEND_KV
}



//
// forwardSnapshotQueryToCSR - GET <csr.endpoint>/ngsi-ld/v1/entities?<qs>.
// Returns the response array (KjArray of entities, API format) or NULL.
//
static KjNode* forwardSnapshotQueryToCSR(LdRegCacheItem* csr, const char* queryString)
{
  if (csr == NULL || csr->endpoint == NULL) return NULL;

  const char* base    = csr->endpoint;
  const char* path    = "/ngsi-ld/v1/entities?";
  int   baseLen = strlen(base);
  int   pathLen = strlen(path);
  int   qsLen   = strlen(queryString);
  char* url     = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + qsLen + 1);

  strcpy(url, base);
  strcpy(url + baseLen, path);
  strcpy(url + baseLen + pathLen, queryString);

  SwRestClientRequest  req;
  SwRestClientResponse resp;

  swRestClientRequestInit(&req, SwVerbGet, url, &swRest.kalloc);
  swRestClientRequestTimeout(&req, 5000, 10000);

  int rc = swRestClientSend(&req, &resp);
  swRestClientResponseCleanup(&resp);   // free the grown response header vector
  if (rc != 0 || resp.statusCode < 200 || resp.statusCode >= 300) return NULL;
  if (resp.body == NULL || resp.bodyLen == 0)                     return NULL;

  char* bodyCopy = (char*) kaAlloc(&swRest.kalloc, resp.bodyLen + 1);
  memcpy(bodyCopy, resp.body, resp.bodyLen);
  bodyCopy[resp.bodyLen] = 0;

  KjNode* treeP = kjParse(swRest.kjsonP, bodyCopy);
  if (treeP != NULL) ldStripAtContext(treeP);
  return treeP;
}



//
// mergeFragmentInto - merge `srcDb` (DB-format entity from a CSR) into
// `destDb` (the snap-tenant's existing DB-format entity for the same
// id). Per-attr / per-datasetId conflict resolution follows § 4.5.5.3
// via ldDistInstanceShouldReplace.
//
// Both trees are mutated in place; the caller is responsible for
// passing trees in a long-enough-lived allocator (typically swRest.kjsonP).
//
static void mergeFragmentInto(KjNode* destDb, KjNode* srcDb, uint64_t nowNs)
{
  if (destDb == NULL || srcDb == NULL || destDb->type != KjObject || srcDb->type != KjObject)
    return;

  // The non-reified entity-level expiresAt is not an Attribute and so takes
  // its own § 4.5.5.3 route: unanimous across versions or gone.
  ldDistExpiresAtReconcile(destDb, srcDb);

  KjNode* srcAttrP = srcDb->value.firstChildP;
  while (srcAttrP != NULL)
  {
    KjNode* nextSrcAttr = srcAttrP->next;

    if (srcAttrP->name == NULL || srcAttrP->name[0] == '@' ||
        strcmp(srcAttrP->name, "id")   == 0 ||
        strcmp(srcAttrP->name, "_id")  == 0 ||
        strcmp(srcAttrP->name, "type") == 0 ||
        srcAttrP->type != KjObject)
    {
      srcAttrP = nextSrcAttr;
      continue;
    }

    KjNode* destAttrP = kjLookup(destDb, srcAttrP->name);
    if (destAttrP == NULL)
    {
      // Attr absent in dest — clone the whole wrapper across.
      kjChildAdd(destDb, kjClone(swRest.kjsonP, srcAttrP));
    }
    else
    {
      // Per-dsKey instance merge.
      for (KjNode* srcInstP = srcAttrP->value.firstChildP; srcInstP != NULL; srcInstP = srcInstP->next)
      {
        KjNode* destInstP = kjLookup(destAttrP, srcInstP->name);
        if (destInstP == NULL)
        {
          if (!ldDistInstanceIsExpired(srcInstP, nowNs))
            kjChildAdd(destAttrP, kjClone(swRest.kjsonP, srcInstP));
        }
        else if (ldDistInstanceShouldReplace(destInstP, srcInstP, nowNs))
        {
          kjChildReplace(destAttrP, destInstP, kjClone(swRest.kjsonP, srcInstP));
        }
      }
    }

    srcAttrP = nextSrcAttr;
  }
}



//
// streamRemoteEntitiesSplit - split-mode counterpart of
// streamRemoteEntitiesIntoSnapshot. For each remote entity:
//   - retrieve the snap-tenant's current state for that id
//   - if absent → create
//   - if present → merge per § 4.5.5.3, then replace
//
static int streamRemoteEntitiesSplit(KjNode* arrayP, Tenant* snapTenantP)
{
  if (arrayP == NULL || arrayP->type != KjArray || snapTenantP == NULL)
    return 0;

  uint64_t nowNs = swRest.requestStartTime;
  int      n     = 0;

  for (KjNode* entityP = arrayP->value.firstChildP; entityP != NULL; entityP = entityP->next)
  {
    if (entityP->type != KjObject) continue;

    KjNode* idP = kjLookup(entityP, "id");
    if (idP == NULL || idP->type != KjString) continue;

    swldExpandTree(entityP, swNgsild.contextP, &swRest.kalloc);
    ldApiEntityToDbModel(entityP, &swRest.kalloc, 0);

    KjNode* existing = NULL;
    int     rc       = db.entityRetrieve(snapTenantP, idP->value.s, &existing);
    if (rc == DB_OK && existing != NULL)
    {
      mergeFragmentInto(existing, entityP, nowNs);
      db.entityReplace(snapTenantP, idP->value.s, existing, NULL);
      n++;
    }
    else
    {
      int wr = db.entityCreate(snapTenantP, idP->value.s, entityP);
      if (wr == DB_OK || wr == DB_ALREADY_EXISTS)
        n++;
    }
  }
  return n;
}



//
// postFilterSnapshotTenant - walk every entity in the snap-tenant,
// apply the snapshotQuery's filter, db.entityDelete the non-matchers.
// Returns the count of survivors. Used only in split-mode capture
// because that's where forwards are unfiltered and the merged set may
// contain entities the query doesn't want.
//
static int postFilterSnapshotTenant(Tenant* snapTenantP, KjNode* queryP)
{
  DbQueryFilter filter = {0};
  queryToFilter(queryP, &filter);

  // Empty filter → fetch all.
  DbQueryFilter empty = {0};
  KjNode* allP = NULL;
  int rc = db.entityQuery(snapTenantP, &empty, &allP);
  if (rc != DB_OK || allP == NULL || allP->type != KjArray)
    return 0;

  int kept = 0;
  KjNode* eP = allP->value.firstChildP;
  while (eP != NULL)
  {
    KjNode* nextP = eP->next;

    KjNode* idP = kjLookup(eP, "id");
    if (idP == NULL) idP = kjLookup(eP, "_id");
    if (idP == NULL || idP->type != KjString) { eP = nextP; continue; }

    bool keep = true;

    if (keep && filter.idV != NULL)
    {
      bool found = false;
      for (int i = 0; filter.idV[i] != NULL; i++)
        if (strcmp(filter.idV[i], idP->value.s) == 0) { found = true; break; }
      if (!found) keep = false;
    }
    if (keep && filter.typeV != NULL && filter.typeV[0] != NULL)
    {
      // entitySelectorsToFilter populates typeV (expanded IRIs); entity's
      // "type" in DB-format is also an expanded IRI (string or string-array).
      KjNode* typeP = kjLookup(eP, "type");
      bool match = false;
      if (typeP != NULL && typeP->type == KjString)
      {
        for (int i = 0; filter.typeV[i] != NULL; i++)
          if (strcmp(filter.typeV[i], typeP->value.s) == 0) { match = true; break; }
      }
      else if (typeP != NULL && typeP->type == KjArray)
      {
        for (KjNode* tP = typeP->value.firstChildP; tP != NULL && !match; tP = tP->next)
        {
          if (tP->type != KjString) continue;
          for (int i = 0; filter.typeV[i] != NULL; i++)
            if (strcmp(filter.typeV[i], tP->value.s) == 0) { match = true; break; }
        }
      }
      if (!match) keep = false;
    }
    if (keep && filter.qExpr != NULL)
    {
      if (!ldEntityMatchQ(eP, filter.qExpr)) keep = false;
    }
    if (keep && filter.scopeExpr != NULL)
    {
      KjNode* scopeP = kjLookup(eP, "scope");
      if (!ldEntityMatchScope(scopeP, filter.scopeExpr)) keep = false;
    }
    if (keep && filter.geoRel != NULL && db.geoMatchFunc != NULL)
    {
      if (!db.geoMatchFunc(eP, filter.geoRel, filter.geometry, filter.coordinates,
                           filter.geoproperty ? filter.geoproperty : "location"))
        keep = false;
    }

    if (!keep)
      db.entityDelete(snapTenantP, idP->value.s);
    else
      kept++;

    eP = nextP;
  }
  return kept;
}



//
// runOneQuery - execute a single Query against the live tenant and
// stream matched entities into the snapshot's own tenant via
// db.entityCreate. Returns the number of entities captured, or -1 on
// DB error.
//
// db.entityQuery returns entities already in DB-model format, which is
// exactly what db.entityCreate expects, so they pass through without
// transformation. The full snapshot dataset never sits in RAM at once —
// the per-query result array is the only RAM buffer (Phase 4 will
// switch to a cursor-based stream so even that goes away).
//
// Distributed capture (Phase 141 no-split / Phase 142 split):
//   - no-split: the contract "every entity fully held by one source"
//     means each CSR returns already-matching entities; the broker's
//     local hits and remote hits are merged via "first writer wins"
//     by id, no per-attribute merge, no post-filter scan.
//   - split:   forwards are unfiltered (`local=true` only); fragments
//     from multiple sources may share an id and must be merged per
//     § 4.5.5.3. After the merge the snap-tenant may hold entities
//     that don't satisfy the snapshotQuery filter — a final post-merge
//     scan deletes the non-matchers.
//
//
// streamRemoteEntitiesIntoSnapshot - given a CSR-returned KjArray of
// API-format entities, expand + storage-wrap each and write to the
// snap-tenant. DB_ALREADY_EXISTS is treated as success ("first writer
// wins" — local hits and earlier CSRs already covered this id).
//
static int streamRemoteEntitiesIntoSnapshot(KjNode* arrayP, Tenant* snapTenantP)
{
  if (arrayP == NULL || arrayP->type != KjArray || snapTenantP == NULL)
    return 0;

  int captured = 0;
  for (KjNode* entityP = arrayP->value.firstChildP; entityP != NULL; entityP = entityP->next)
  {
    if (entityP->type != KjObject) continue;

    KjNode* idP = kjLookup(entityP, "id");
    if (idP == NULL || idP->type != KjString) continue;

    // CSR responses are in API form — expand short names + wrap attrs
    // into the broker's storage format that db.entityCreate expects.
    swldExpandTree(entityP, swNgsild.contextP, &swRest.kalloc);
    ldApiEntityToDbModel(entityP, &swRest.kalloc, 0);

    int wr = db.entityCreate(snapTenantP, idP->value.s, entityP);
    if (wr == DB_OK || wr == DB_ALREADY_EXISTS)
      captured++;
  }
  return captured;
}



static int runOneQuery(LdSnapshotCacheItem* itemP,
                       KjNode*              queryP,
                       Tenant*              tenantP)
{
  if (itemP->snapTenantP == NULL) return -1;
  Tenant* snapTenantP = (Tenant*) itemP->snapTenantP;

  bool splitMode = swNgsild.splitEntitiesSet ? swNgsild.splitEntitiesVal : ldSplitEntities;

  // 1) Local capture.
  //    no-split → full filter (each entity is fully held by one source,
  //                so the local DB returns the exact match set).
  //    split    → only id/idPattern (other filters are post-merge in
  //                this regime — every fragment must reach the snap-
  //                tenant before we can decide whether to keep it).
  DbQueryFilter filter = {0};
  queryToFilter(queryP, &filter);

  DbQueryFilter localFilter = filter;
  if (splitMode)
  {
    localFilter.typeV     = NULL;
    localFilter.typeExpr  = NULL;
    localFilter.qExpr     = NULL;
    localFilter.scopeExpr = NULL;
    localFilter.geoRel    = NULL;
    localFilter.geometry    = NULL;
    localFilter.coordinates = NULL;
    localFilter.geoproperty = NULL;
  }

  KjNode* arrayP = NULL;
  int rc = db.entityQuery(tenantP, &localFilter, &arrayP);
  if (rc != DB_OK)
    return -1;

  int n = 0;
  if (arrayP != NULL && arrayP->type == KjArray)
  {
    for (KjNode* entityP = arrayP->value.firstChildP; entityP != NULL; entityP = entityP->next)
    {
      KjNode* idP = kjLookup(entityP, "id");
      if (idP == NULL) idP = kjLookup(entityP, "_id");
      if (idP == NULL || idP->type != KjString) continue;

      int wr = db.entityCreate(snapTenantP, idP->value.s, entityP);
      if (wr == DB_OK || wr == DB_ALREADY_EXISTS)
        n++;
    }
  }

  // 2) Distributed capture — across all four reg modes uniformly.
  //    The forward URL differs by split mode; the merge strategy on
  //    arrival also differs (first-writer-wins vs § 4.5.5.3 merge).
  if (tenantP == NULL || tenantP->regCacheP == NULL)
  {
    if (splitMode)
      return postFilterSnapshotTenant(snapTenantP, queryP);
    return n;
  }

  const char* qs = splitMode
                     ? buildSplitForwardQs(queryP, &swRest.kalloc)
                     : buildQueryStringFromSnapshotQuery(queryP, &swRest.kalloc);
  LdRegMode   modes[] = { LdRegModeExclusive, LdRegModeRedirect, LdRegModeInclusive, LdRegModeAuxiliary };
  LdRegCache* regC    = (LdRegCache*) tenantP->regCacheP;

  for (int m = 0; m < 4; m++)
  {
    LdRegCacheItem** matchV = NULL;
    int              matchN = ldRegCacheMatchForRetrieve(regC, NULL, NULL, modes[m], &matchV);

    for (int i = 0; i < matchN; i++)
    {
      KjNode* remoteArr = forwardSnapshotQueryToCSR(matchV[i], qs);
      if (remoteArr == NULL || remoteArr->type != KjArray) continue;

      if (splitMode) n += streamRemoteEntitiesSplit(remoteArr, snapTenantP);
      else           n += streamRemoteEntitiesIntoSnapshot(remoteArr, snapTenantP);
    }

    ldRegCacheMatchRelease(matchV, matchN);
  }

  // 3) Split-mode only: post-merge filter scan over the snap-tenant.
  //    Returns the survivor count, which is the value the caller wants
  //    for snapshotQueriesDetails (no-split skipped this and returns
  //    the running tally of streamed entities).
  if (splitMode)
    return postFilterSnapshotTenant(snapTenantP, queryP);

  return n;
}



//
// pickStatus - aggregate per-query outcomes into the snapshot status.
//
// § 5.16.1.4:
//   - success: all queries successful AND at least one yielded a result
//   - partial: at least one query yielded a result, others failed/empty
//   - empty:   at least one ran successfully, but ALL yielded nothing
//   - failure: nothing else applies (no successful runs at all, or no queries)
//
static const char* pickStatus(int nSuccess, int nEmpty, int nFailure)
{
  if (nSuccess + nEmpty + nFailure == 0) return "failure";

  if (nSuccess > 0 && nFailure == 0 && nEmpty == 0) return "success";
  if (nSuccess > 0)                                  return "partial";
  if (nFailure == 0 && nEmpty > 0)                   return "empty";
  return "failure";
}



//
// statusFromString - mirror the string back onto the cache item enum.
//
static LdSnapshotStatus statusFromString(const char* s)
{
  if (strcmp(s, "success") == 0) return LdSnapshotSuccess;
  if (strcmp(s, "partial") == 0) return LdSnapshotPartial;
  if (strcmp(s, "empty")   == 0) return LdSnapshotEmpty;
  return LdSnapshotFailure;
}



bool ldSnapshotExecQueries(LdSnapshotCache*     cacheP,
                           LdSnapshotCacheItem* itemP,
                           Tenant*              tenantP)
{
  (void) cacheP;
  if (itemP == NULL || itemP->tree == NULL) return false;

  KjNode* qListP   = kjLookup(itemP->tree, "snapshotQueries");
  // Build the details array directly in the cache's persistent allocator
  // (NULL = malloc) so the data outlives the request / worker thread.
  KjNode* detailsP = kjArray(NULL, "snapshotQueriesDetails");
  int nSuccess = 0, nEmpty = 0, nFailure = 0;

  if (qListP != NULL && qListP->type == KjArray)
  {
    for (KjNode* queryP = qListP->value.firstChildP; queryP != NULL; queryP = queryP->next)
    {
      KjNode* detail = kjObject(NULL, NULL);

      const char* result;
      int matched = runOneQuery(itemP, queryP, tenantP);
      if      (matched > 0)  { result = "success"; nSuccess++; }
      else if (matched == 0) { result = "empty";   nEmpty++;   }
      else                    { result = "failure"; nFailure++; }

      kjChildAdd(detail, kjString(NULL, "resultStatus", (char*) result));
      kjChildAdd(detailsP, detail);
    }
  }

  // Append snapshotQueriesDetails to itemP->tree if any queries ran.
  if (detailsP->value.firstChildP != NULL)
  {
    KjNode* existing = kjLookup(itemP->tree, "snapshotQueriesDetails");
    if (existing != NULL)
    {
      // itemP->tree is an all-malloc clone — kjChildRemove only unlinks, so
      // free the previous details to avoid orphaning it on a re-run.
      kjChildRemove(itemP->tree, existing);
      kjFree(existing);
    }
    kjChildAdd(itemP->tree, detailsP);
  }
  else
    kjFree(detailsP);   // empty (no queries ran) — never grafted, so free it

  const char* status = pickStatus(nSuccess, nEmpty, nFailure);

  // Update snapshotStatus on the cached tree.
  KjNode* sCachedP = kjLookup(itemP->tree, "snapshotStatus");
  if (sCachedP != NULL && sCachedP->type == KjString)
    sCachedP->value.s = (char*) status;

  itemP->status = statusFromString(status);

  return true;
}

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
#include "swJsonld/swldExpand.h"                     // swldExpand
#include "swJsonld/swldExpandTree.h"                 // swldExpandTree
#include "swJsonld/swldCompact.h"                    // swldCompact
#include "swJsonld/swldInit.h"                       // swldCoreContext
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldParamsValidate.h"               // ldParamsValidate
#include "swNgsild/ldOrderSort.h"                    // ldOrderSort
#include "swNgsild/ldStripAtContext.h"              // ldStripAtContext
#include "swNgsild/ldEntityMatch.h"                  // ldEntityMatchType, ldEntityMatchQ, ldEntityMatchScope
#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieve
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "swNgsild/ldDistOp.h"                       // ldDistOpCsrWouldLoop
#include "swNgsild/LdEntityMap.h"                    // LdEntityMap, LdEntityMapStore
#include "swNgsild/ldEntityMap.h"                    // ldEntityMapCreate, ldEntityMapAddEntry, ldEntityMapToTree

#include "db/DbDriver.h"                             // db, DB_OK
#include "db/Tenant.h"                               // Tenant

#include "linkedEntities/ldLinkedEntities.h"         // ldLinkedEntitiesExpandArrayFlat / Inline

#include "serviceRoutines/getEntities.h"             // Own interface



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
                                      const char*     ownAlias)
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

  int status = ldDistOpSendReceive(csr, SwVerbGet, url, NULL, 0, ownAlias,
                                    &errDetail, &respBody, &respBodyLen);
  if (status < 200 || status >= 300 || respBody == NULL || respBodyLen == 0)
    return NULL;

  KjNode* treeP = kjParse(swRest.kjsonP, respBody);
  if (treeP == NULL)
    return NULL;

  swldExpandTree(treeP, &swRest.kalloc);
  ldStripAtContext(treeP);
  apiAttrToStorageWrap(treeP);
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
// getEntities -
//
bool getEntities(void)
{
  //
  // Early exit if paramHook already set an error
  //
  if (swRest.out.problemType != NULL)
    return true;

  //
  // EntityMap-based pagination: if entityMap=<mapId>, fetch entities
  // from the frozen map instead of re-querying.
  //
  if (swNgsild.entityMapId != NULL)
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
    // § 5.2.39 pagination: each map entry records a list of sources that
    // contribute to the entity. Retrieve from each listed source — local
    // "@none" via db.entityRetrieve, remote via HTTP GET on the CSR
    // identified by regId. Do NOT consult the registration cache for
    // routing — the whole point of the map is a stable snapshot of
    // "where each entity is" at map-creation time, immune to later reg
    // changes.
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
            partialP = retrieveEntityFromCSR(csr, entryP->entityId, ownAlias);
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

    // Count header
    if (swNgsild.count)
    {
      char* countStr = (char*) kaAlloc(&swRest.kalloc, 32);
      snprintf(countStr, 32, "%d", mapP->entryCount);
      SwRestKeyValue* hV = swRest.out.headerV;
      int hix = swRest.out.headerCount;
      hV[hix].key   = "NGSILD-Results-Count";
      hV[hix].value = countStr;
      swRest.out.headerCount++;
    }

    // Pagination Link header
    bool hasMore = (offset + added < mapP->entryCount);
    if (hasMore)
    {
      char* nextUrl = (char*) kaAlloc(&swRest.kalloc, 256);
      snprintf(nextUrl, 256, "</ngsi-ld/v1/entities?entityMap=%s&offset=%d&limit=%d>; rel=\"next\"; type=\"application/json\"",
               swNgsild.entityMapId, offset + limit, limit);
      SwRestKeyValue* hV = swRest.out.headerV;
      int hix = swRest.out.headerCount;
      hV[hix].key   = "Link";
      hV[hix].value = nextUrl;
      swRest.out.headerCount++;
    }

    // Apply pick/omit
    if (swNgsild.pickV != NULL || swNgsild.omitV != NULL)
    {
      for (KjNode* ep = arrayP->value.firstChildP; ep != NULL; ep = ep->next)
        ldPickOmit(ep, swNgsild.pickV, swNgsild.omitV);
    }

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

  //
  // Cross-parameter validation (limit=0 requires count, etc.)
  //
  if (ldParamsValidate())
    return true;

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
      const char* baseQs = NULL;
      const char* ownAlias = ldCsourceAliasForTenant(tP->name, &swRest.kalloc);

      for (int m = 0; m < 4; m++)
      {
        matchV = NULL;
        // Split mode: match ALL registrations regardless of type
        matchN = ldRegCacheMatchForRetrieve((LdRegCache*) tP->regCacheP,
                                            NULL,
                                            splitModeSetting ? NULL : swNgsild.typeV,
                                            modes[m], &matchV);
        if (matchN == 0)
          continue;

        // Registrations matched — activate split mode if configured
        if (splitModeSetting && !splitMode)
        {
          splitMode = true;
          // Re-query local WITHOUT filters (need all candidate entities)
          DbQueryFilter splitFilter = {0};
          splitFilter.idV       = filter.idV;
          splitFilter.idPattern = filter.idPattern;
          splitFilter.limit     = 1000000;  // effectively unlimited
          // No type, q, geoQ, scopeQ — applied post-assembly
          arrayP = NULL;
          db.entityQuery((Tenant*) swNgsild.tenantP, &splitFilter, &arrayP);

          // Re-stamp srcMap with the new (unfiltered) local set.
          srcMapStampLocalFrom(srcMap, arrayP);
        }

        if (baseQs == NULL)
          baseQs = splitMode ? "" : buildQueryString();

        for (int i = 0; i < matchN; i++)
        {
          LdRegCacheItem* csr = matchV[i];
          if (csr->endpoint == NULL)
            continue;

          // Proactive loop-detect (§ 5.12): CSR alias known + in chain → skip
          if (ldDistOpCsrWouldLoop(csr, ownAlias))
            continue;

          //
          // Split mode: forward once per CSR (no filters, no per-info pick)
          // No-split: per-RegistrationInfo dispatch with type + pick
          //
          const char* fullQs;

          if (splitMode)
          {
            // Split: forward with no filters — get all entities from this CSR
            fullQs = baseQs;  // empty string
          }
          else
          {
            // No-split: per-RegistrationInfo dispatch
            // For simplicity, forward once per CSR with base query + combined pick
            // (the per-info-entry logic applies type + pick constraints)
            fullQs = baseQs;

            // Build per-info pick for the first matching info entry
            for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
            {
              // Type check
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

              // Build pick param
              const char* pickParam = "";
              if (riP->propertyNamesV != NULL || riP->relationshipNamesV != NULL)
              {
                int totalLen = 0, cnt = 0;
                char** lists[] = { riP->propertyNamesV, riP->relationshipNamesV, NULL };
                for (int li = 0; lists[li] != NULL; li++)
                  for (int a = 0; lists[li][a] != NULL; a++)
                  {
                    const char* c = swldCompact(swldCoreContext(), lists[li][a]);
                    totalLen += strlen(c ? c : lists[li][a]) + 1;
                    cnt++;
                  }
                if (cnt > 0)
                {
                  char* buf = (char*) kaAlloc(&swRest.kalloc, 6 + totalLen + 1);
                  strcpy(buf, "&pick=");
                  int pos = 6;
                  for (int li = 0; lists[li] != NULL; li++)
                    for (int a = 0; lists[li][a] != NULL; a++)
                    {
                      if (pos > 6) buf[pos++] = ',';
                      const char* c = swldCompact(swldCoreContext(), lists[li][a]);
                      const char* n = c ? c : lists[li][a];
                      strcpy(buf + pos, n);
                      pos += strlen(n);
                    }
                  buf[pos] = 0;
                  pickParam = buf;
                }
              }

              int bLen = strlen(baseQs), pLen = strlen(pickParam);
              char* combined = (char*) kaAlloc(&swRest.kalloc, bLen + pLen + 1);
              strcpy(combined, baseQs);
              strcpy(combined + bLen, pickParam);
              fullQs = combined;
              break;  // use first matching info entry
            }
          }

          {
            KjNode* remoteArray = forwardQueryToCSR(csr, fullQs);
            if (remoteArray == NULL || remoteArray->type != KjArray)
              continue;

            // Merge remote entities — dedup by ID
            for (KjNode* remoteEntity = remoteArray->value.firstChildP; remoteEntity != NULL; )
            {
              KjNode* nextRemote = remoteEntity->next;

              KjNode* remoteIdP = kjLookup(remoteEntity, "id");
              if (remoteIdP == NULL || remoteIdP->type != KjString)
              {
                remoteEntity = nextRemote;
                continue;
              }

              // Find existing entity with same ID
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
              swldExpandTree(remoteEntity, &swRest.kalloc);
              apiAttrToStorageWrap(remoteEntity);

              if (existingP == NULL)
              {
                // New entity — add to results
                kjChildAdd(arrayP, remoteEntity);
                srcMapAdd(srcMap, remoteIdP->value.s, csr->regId);
              }
              else if (splitMode)
              {
                // Split-mode merge: this CSR contributed attrs to an
                // entity that also exists locally (or in another CSR).
                // Record the regId alongside whatever was already there.
                srcMapAdd(srcMap, remoteIdP->value.s, csr->regId);
                // Split mode: merge remote attrs into existing entity per
                // § 4.5.5.3 — instance-level conflict resolution per
                // (attrName, datasetId). Both trees are in storage format.
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
                    // Attr not in dest — move whole wrapper
                    srcAttrP->next = NULL;
                    kjChildAdd(existingP, srcAttrP);
                  }
                  else
                  {
                    // Attr exists — merge per dsKey instance
                    for (KjNode* srcInstP = srcAttrP->value.firstChildP; srcInstP != NULL; )
                    {
                      KjNode* nextSrcInst = srcInstP->next;
                      KjNode* destInstP = kjLookup(destAttrP, srcInstP->name);

                      if (destInstP == NULL)
                      {
                        // dsKey not in dest — add
                        srcInstP->next = NULL;
                        kjChildAdd(destAttrP, srcInstP);
                      }
                      // else: both have this dsKey — keep dest's (first wins
                      // without timestamps; full § 4.5.5.3 would compare
                      // observedAt/modifiedAt here)

                      srcInstP = nextSrcInst;
                    }
                  }

                  srcAttrP = nextSrcAttr;
                }
              }
              // else: no-split mode, duplicate — skip

              remoteEntity = nextRemote;
            }
          }
        }

        free(matchV);
      }
    }

    //
    // Split mode post-assembly: apply filters on assembled entities.
    //
    if (splitMode && arrayP != NULL)
    {
      KjNode* entityP = arrayP->value.firstChildP;
      while (entityP != NULL)
      {
        KjNode* nextP = entityP->next;
        bool keep = true;

        // Type filter (using type expression for full selector support)
        if (keep && swNgsild.typeExpr != NULL)
        {
          KjNode* typeP = kjLookup(entityP, "type");
          if (!ldEntityMatchType(typeP, swNgsild.typeExpr))
            keep = false;
        }

        // q-filter
        if (keep && swNgsild.qExpr != NULL)
        {
          if (!ldEntityMatchQ(entityP, swNgsild.qExpr))
            keep = false;
        }

        // scopeQ filter
        if (keep && swNgsild.scopeExpr != NULL)
        {
          KjNode* scopeP = kjLookup(entityP, "scope");
          if (!ldEntityMatchScope(scopeP, swNgsild.scopeExpr))
            keep = false;
        }

        // geoQ: use the DB driver's registered geo match callback
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

      // Add NGSILD-EntityMap header with the map's URL
      char* mapUrl = (char*) kaAlloc(&swRest.kalloc, 128);
      snprintf(mapUrl, 128, "/ngsi-ld/v1/entityMaps/%s", mapP->mapId);

      SwRestKeyValue* hV = swRest.out.headerV;
      int ix = swRest.out.headerCount;
      hV[ix].key   = "NGSILD-EntityMap";
      hV[ix].value = mapUrl;
      swRest.out.headerCount++;

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

    SwRestKeyValue* hV = swRest.out.headerV;
    int ix = swRest.out.headerCount;
    hV[ix].key   = "NGSILD-Results-Count";
    hV[ix].value = countStr;
    swRest.out.headerCount++;
  }

  //
  // Pagination: trim to limit and add Link header with next/prev
  //
  bool hasMore = ldPaginationTrim(arrayP, swNgsild.limit);
  ldPaginationLinkHeader(hasMore);

  //
  // Apply pick/omit attribute projection
  //
  if (swNgsild.pickV != NULL || swNgsild.omitV != NULL)
  {
    for (KjNode* entityP = arrayP->value.firstChildP; entityP != NULL; entityP = entityP->next)
      ldPickOmit(entityP, swNgsild.pickV, swNgsild.omitV);
  }

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

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
#include "kjson/kjBuilder.h"                         // kjArray, kjObject, kjChildAdd
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
#include "swNgsild/ldEntityMatch.h"                  // ldEntityMatchType, ldEntityMatchQ, ldEntityMatchScope
#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieve
#include "swNgsild/LdEntityMap.h"                    // LdEntityMap, LdEntityMapStore
#include "swNgsild/ldEntityMap.h"                    // ldEntityMapCreate, ldEntityMapAddEntry, ldEntityMapToTree

#include "db/DbDriver.h"                             // db, DB_OK
#include "db/Tenant.h"                               // Tenant

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
  const char* path = "/entities?";
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
  return treeP;  // KjArray of entities (in API format)
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
        }

        if (baseQs == NULL)
          baseQs = splitMode ? "" : buildQueryString();

        for (int i = 0; i < matchN; i++)
        {
          LdRegCacheItem* csr = matchV[i];
          if (csr->endpoint == NULL)
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
              }
              else if (splitMode)
              {
                // Split mode: merge remote attrs into existing entity
                // (add attrs not already present — simple overlay)
                for (KjNode* attrP = remoteEntity->value.firstChildP; attrP != NULL; )
                {
                  KjNode* nextAttr = attrP->next;
                  if (attrP->name != NULL &&
                      strcmp(attrP->name, "id") != 0 &&
                      strcmp(attrP->name, "type") != 0 &&
                      attrP->name[0] != '@' &&
                      kjLookup(existingP, attrP->name) == NULL)
                  {
                    attrP->next = NULL;
                    kjChildAdd(existingP, attrP);
                  }
                  attrP = nextAttr;
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

        // geoQ: use the registered geoMatch function (GEOS-based)
        if (keep && swNgsild.geoRel != NULL && tP != NULL && tP->subCacheP != NULL)
        {
          // geoQ post-filter requires the same geo matching as subscriptions.
          // For now, skip — geoQ post-assembly is complex (needs the geo
          // match function from the DB plugin). Will be added when needed.
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
  if (swNgsild.entityMap)
  {
    Tenant* tP = (Tenant*) swNgsild.tenantP;

    if (tP != NULL && tP->entityMapStoreP != NULL)
    {
      // Purge expired maps first
      ldEntityMapPurgeExpired((LdEntityMapStore*) tP->entityMapStoreP);

      // Default lifetime: 5 minutes
      LdEntityMap* mapP = ldEntityMapCreate((LdEntityMapStore*) tP->entityMapStoreP,
                                             5ULL * 60 * 1000000000ULL, tP);

      // Walk the sorted array, add each entity ID to the map
      for (KjNode* entityP = arrayP->value.firstChildP; entityP != NULL; entityP = entityP->next)
      {
        KjNode* idP = kjLookup(entityP, "id");
        if (idP != NULL && idP->type == KjString)
        {
          const char* src = "@none";  // TODO: track actual source per entity
          ldEntityMapAddEntry(mapP, idP->value.s, &src, 1);
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

  swRest.out.responseTree = arrayP;
  return true;
}

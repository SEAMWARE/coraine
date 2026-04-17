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
  // Query the local database
  //
  KjNode* arrayP = NULL;
  int     r      = db.entityQuery((Tenant*) swNgsild.tenantP, &filter, &arrayP);

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error querying entities");
    return true;
  }

  //
  // Distributed query (no-split mode): if registrations match and ?local=true
  // is not set, forward the query to each matching CSR and merge results.
  // Entity IDs are collected into an EntityMap for pagination.
  //
  if (swNgsild.local == false)
  {
    Tenant*           tP    = (Tenant*) swNgsild.tenantP;
    LdRegCacheItem**  matchV = NULL;
    int               matchN = 0;

    // Match all four CSR modes (no-split: each entity fully at one source,
    // so the merge logic is the same — forward + dedup by entity ID).
    // Processing order per § 4.3.6: exclusive → redirect → inclusive → auxiliary.
    //
    if (tP != NULL && tP->regCacheP != NULL)
    {
      LdRegMode modes[] = { LdRegModeExclusive, LdRegModeRedirect, LdRegModeInclusive, LdRegModeAuxiliary };
      const char* qs = NULL;

      for (int m = 0; m < 4; m++)
      {
        matchV = NULL;
        matchN = ldRegCacheMatchForRetrieve((LdRegCache*) tP->regCacheP,
                                            NULL, swNgsild.typeV,
                                            modes[m], &matchV);
        if (matchN == 0)
          continue;

        if (qs == NULL)
          qs = buildQueryString();

        for (int i = 0; i < matchN; i++)
        {
          KjNode* remoteArray = forwardQueryToCSR(matchV[i], qs);
          if (remoteArray == NULL || remoteArray->type != KjArray)
            continue;

          for (KjNode* remoteEntity = remoteArray->value.firstChildP; remoteEntity != NULL; )
          {
            KjNode* nextRemote = remoteEntity->next;

            KjNode* remoteIdP = kjLookup(remoteEntity, "id");
            if (remoteIdP == NULL || remoteIdP->type != KjString)
            {
              remoteEntity = nextRemote;
              continue;
            }

            bool duplicate = false;
            for (KjNode* existingP = arrayP->value.firstChildP; existingP != NULL; existingP = existingP->next)
            {
              KjNode* existIdP = kjLookup(existingP, "id");
              if (existIdP != NULL && existIdP->type == KjString && strcmp(existIdP->value.s, remoteIdP->value.s) == 0)
              {
                duplicate = true;
                break;
              }
            }

            if (!duplicate)
            {
              remoteEntity->next = NULL;
              swldExpandTree(remoteEntity, &swRest.kalloc);
              apiAttrToStorageWrap(remoteEntity);
              kjChildAdd(arrayP, remoteEntity);
            }

            remoteEntity = nextRemote;
          }
        }

        free(matchV);
      }
    }
  }

  //
  // Sort by orderBy before pagination (§ 4.23)
  //
  if (swNgsild.orderByV != NULL && swNgsild.orderByCount > 0)
    ldOrderSort(arrayP, swNgsild.orderByV, swNgsild.orderByCount);

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

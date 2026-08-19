//
// FILE            ldSnapshotRead.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Snapshot-aware read paths — see header.
//
// Each snapshot owns a dedicated DB tenant (LdSnapshotCacheItem.snapTenantP)
// holding its frozen entity bodies. Read paths swap that tenant in for
// the live tenant, then call the standard db.entityRetrieve / db.entityQuery
// — orderBy, q, pick/omit, pagination all just work, no special code.
//
#include <stdbool.h>                                     // bool
#include <string.h>                                      // strcasecmp, strcmp

#include "kjson/KjNode.h"                                // KjNode

#include "corRest/CorRestState.h"                          // corRest
#include "corRest/corRestOutHeader.h"                      // corRestOutHeaderAdd

#include "corJsonld/corLdExpand.h"                         // corLdExpand

#include "corNgsild/corNgsild.h"                           // ldError, corNgsild
#include "corNgsild/LdProblem.h"                          // LD_ERROR_*
#include "corNgsild/LdSnapshotCache.h"                    // LdSnapshotCache, ldSnapshotCacheItemLookup
#include "corNgsild/ldOrderSort.h"                        // ldOrderSort
#include "corNgsild/ldPickOmit.h"                         // ldPickOmit
#include "corNgsild/ldPagination.h"                       // ldPaginationTrim, ldPaginationLinkHeader

#include "db/DbDriver.h"                                 // db, DB_OK, DB_NOT_FOUND
#include "db/DbQueryFilter.h"                            // DbQueryFilter
#include "db/Tenant.h"                                   // Tenant

#include "serviceRoutines/ldSnapshotRead.h"              // Own interface



static const char* readSnapshotIdHeader(void)
{
  for (int i = 0; i < corRest.in.httpHeaderCount; i++)
  {
    if (strcasecmp(corRest.in.httpHeaderV[i].key, "NGSILD-Snapshot") == 0)
      return corRest.in.httpHeaderV[i].value;
  }
  return NULL;
}



LdSnapshotCacheItem* ldSnapshotItemFromHeader(bool* seenP)
{
  *seenP = false;

  const char* id = readSnapshotIdHeader();
  if (id == NULL || id[0] == 0)
    return NULL;

  *seenP = true;

  Tenant* tP = (Tenant*) corNgsild.tenantP;
  LdSnapshotCache* cacheP = (tP != NULL) ? (LdSnapshotCache*) tP->snapshotCacheP : NULL;
  if (cacheP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "Snapshot '%s' not found", id);
    return NULL;
  }

  LdSnapshotCacheItem* itemP = ldSnapshotCacheItemLookup(cacheP, id);
  if (itemP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "Snapshot '%s' not found", id);
    return NULL;
  }

  itemP->lastUsedAt = corRest.requestStartTime;
  corRestOutHeaderAdd("NGSILD-Snapshot", itemP->id);
  return itemP;
}



bool snapshotGetEntity(LdSnapshotCacheItem* itemP, const char* entityId)
{
  Tenant* snapTenantP = (Tenant*) itemP->snapTenantP;
  if (snapTenantP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "entity '%s' not found", entityId);
    return true;
  }

  KjNode* entityP = NULL;
  int rc = db.entityRetrieve(snapTenantP, entityId, &entityP);
  if (rc == DB_NOT_FOUND || entityP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "entity '%s' not found", entityId);
    return true;
  }
  if (rc != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "database error retrieving entity '%s'", entityId);
    return true;
  }

  if (corNgsild.pickV != NULL || corNgsild.omitV != NULL)
    ldPickOmit(entityP, corNgsild.pickV, corNgsild.omitV);

  corRest.out.responseTree = entityP;
  return true;
}



bool snapshotGetEntities(LdSnapshotCacheItem* itemP)
{
  Tenant* snapTenantP = (Tenant*) itemP->snapTenantP;
  if (snapTenantP == NULL)
  {
    corRest.out.responseTree = NULL;
    return true;
  }

  // Reuse the same filter the live path builds in getEntities, just
  // re-pointed at the snap tenant. No distop fan-out (§ 5.5.15 forces
  // local scope), so this is a single db.entityQuery call.
  DbQueryFilter filter = {0};
  filter.idV       = corNgsild.idV;
  filter.idPattern = corNgsild.idPattern;
  filter.typeV     = corNgsild.typeV;
  filter.typeExpr  = corNgsild.typeExpr;
  filter.scopeExpr = corNgsild.scopeExpr;
  filter.qExpr     = corNgsild.qExpr;
  filter.geoRel      = corNgsild.geoRel;
  filter.geometry    = corNgsild.geometry;
  filter.coordinates = corNgsild.coordinates;
  filter.geoproperty = corNgsild.geoproperty
                         ? corNgsild.geoproperty
                         : corLdExpand(corNgsild.contextP, "location", &corRest.kalloc, NULL, NULL);
  filter.limit  = (corNgsild.limit > 0) ? corNgsild.limit + 1 : 0;
  filter.offset = corNgsild.offset;
  filter.count  = corNgsild.count;

  KjNode* arrayP = NULL;
  int rc = db.entityQuery(snapTenantP, &filter, &arrayP);
  if (rc != DB_OK || arrayP == NULL)
  {
    corRest.out.responseTree = NULL;
    return true;
  }

  if (corNgsild.orderByV != NULL && corNgsild.orderByCount > 0)
    ldOrderSort(arrayP, corNgsild.orderByV, corNgsild.orderByCount, corNgsild.collation);

  // § 7.4.2.2: no prev/next pointers for a page that is empty AND has nothing
  // more pending; keep next when more pages remain (hasMore).
  bool hasMore = ldPaginationTrim(arrayP, corNgsild.limit);
  if ((arrayP != NULL && arrayP->value.firstChildP != NULL) || hasMore)
    ldPaginationLinkHeader(hasMore);

  if (corNgsild.pickV != NULL || corNgsild.omitV != NULL)
  {
    for (KjNode* entityP = arrayP->value.firstChildP; entityP != NULL; entityP = entityP->next)
      ldPickOmit(entityP, corNgsild.pickV, corNgsild.omitV);
  }

  corRest.out.responseTree = arrayP;
  return true;
}



bool ldSnapshotWriteGuard(void)
{
  if (readSnapshotIdHeader() == NULL)
    return true;

  if (corRest.in.verb == CorVerbGet || corRest.in.verb == CorVerbHead)
    return true;

  ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Operation Not Supported",
          "NGSILD-Snapshot header cannot be combined with write operations or with subscription / registration creation; snapshots are immutable in NGSI-LD v1.9.1");
  return false;
}

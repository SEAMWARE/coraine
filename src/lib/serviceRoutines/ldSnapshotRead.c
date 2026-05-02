//
// FILE            ldSnapshotRead.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
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

#include "swRest/SwRestState.h"                          // swRest
#include "swRest/swRestOutHeader.h"                      // swRestOutHeaderAdd

#include "swJsonld/swldExpand.h"                         // swldExpand

#include "swNgsild/swNgsild.h"                           // ldError, swNgsild
#include "swNgsild/LdProblem.h"                          // LD_ERROR_*
#include "swNgsild/LdSnapshotCache.h"                    // LdSnapshotCache, ldSnapshotCacheItemLookup
#include "swNgsild/ldOrderSort.h"                        // ldOrderSort
#include "swNgsild/ldPickOmit.h"                         // ldPickOmit
#include "swNgsild/ldPagination.h"                       // ldPaginationTrim, ldPaginationLinkHeader

#include "db/DbDriver.h"                                 // db, DB_OK, DB_NOT_FOUND
#include "db/DbQueryFilter.h"                            // DbQueryFilter
#include "db/Tenant.h"                                   // Tenant

#include "serviceRoutines/ldSnapshotRead.h"              // Own interface



static const char* readSnapshotIdHeader(void)
{
  for (int i = 0; i < swRest.in.httpHeaderCount; i++)
  {
    if (strcasecmp(swRest.in.httpHeaderV[i].key, "NGSILD-Snapshot") == 0)
      return swRest.in.httpHeaderV[i].value;
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

  Tenant* tP = (Tenant*) swNgsild.tenantP;
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

  itemP->lastUsedAt = swRest.requestStartTime;
  swRestOutHeaderAdd("NGSILD-Snapshot", itemP->id);
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

  if (swNgsild.pickV != NULL || swNgsild.omitV != NULL)
    ldPickOmit(entityP, swNgsild.pickV, swNgsild.omitV);

  swRest.out.responseTree = entityP;
  return true;
}



bool snapshotGetEntities(LdSnapshotCacheItem* itemP)
{
  Tenant* snapTenantP = (Tenant*) itemP->snapTenantP;
  if (snapTenantP == NULL)
  {
    swRest.out.responseTree = NULL;
    return true;
  }

  // Reuse the same filter the live path builds in getEntities, just
  // re-pointed at the snap tenant. No distop fan-out (§ 5.5.15 forces
  // local scope), so this is a single db.entityQuery call.
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
  filter.geoproperty = swNgsild.geoproperty
                         ? swNgsild.geoproperty
                         : swldExpand(swNgsild.contextP, "location", &swRest.kalloc, NULL, NULL);
  filter.limit  = (swNgsild.limit > 0) ? swNgsild.limit + 1 : 0;
  filter.offset = swNgsild.offset;
  filter.count  = swNgsild.count;

  KjNode* arrayP = NULL;
  int rc = db.entityQuery(snapTenantP, &filter, &arrayP);
  if (rc != DB_OK || arrayP == NULL)
  {
    swRest.out.responseTree = NULL;
    return true;
  }

  if (swNgsild.orderByV != NULL && swNgsild.orderByCount > 0)
    ldOrderSort(arrayP, swNgsild.orderByV, swNgsild.orderByCount);

  bool hasMore = ldPaginationTrim(arrayP, swNgsild.limit);
  ldPaginationLinkHeader(hasMore);

  if (swNgsild.pickV != NULL || swNgsild.omitV != NULL)
  {
    for (KjNode* entityP = arrayP->value.firstChildP; entityP != NULL; entityP = entityP->next)
      ldPickOmit(entityP, swNgsild.pickV, swNgsild.omitV);
  }

  swRest.out.responseTree = arrayP;
  return true;
}



bool ldSnapshotWriteGuard(void)
{
  if (readSnapshotIdHeader() == NULL)
    return true;

  if (swRest.in.verb == SwVerbGet || swRest.in.verb == SwVerbHead)
    return true;

  ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Operation Not Supported",
          "NGSILD-Snapshot header cannot be combined with write operations or with subscription / registration creation; snapshots are immutable in NGSI-LD v1.9.1");
  return false;
}

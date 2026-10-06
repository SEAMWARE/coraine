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
#include <string.h>                                      // strcasecmp, strcmp, strncmp, strlen, strstr

#include "corTree/CorNode.h"                             // CorNode

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

  //
  // PINNED for the rest of the request: the caller queries the snapshot's tenant, and the id
  // goes out in a response header. It was a plain lookup, and a DELETE of the snapshot meanwhile
  // destroyed that tenant under the query. Unpinned by the post-response hook
  // (ldSnapshotRequestRelease).
  //
  LdSnapshotCacheItem* itemP = ldSnapshotCacheItemLookupPinned(cacheP, id);
  if (itemP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "Snapshot '%s' not found", id);
    return NULL;
  }

  ldSnapshotRequestRelease();          // one routed snapshot per request
  corNgsild.snapshotPinned = itemP;

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

  CorNode* entityP = NULL;
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

  CorNode* arrayP = NULL;
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
  if ((arrayP != NULL && arrayP->value.head != NULL) || hasMore)
    ldPaginationLinkHeader(hasMore);

  if (corNgsild.pickV != NULL || corNgsild.omitV != NULL)
  {
    for (CorNode* entityP = arrayP->value.head; entityP != NULL; entityP = entityP->next)
      ldPickOmit(entityP, corNgsild.pickV, corNgsild.omitV);
  }

  corRest.out.responseTree = arrayP;
  return true;
}



#if COR_FEATURE_SNAPSHOT_WRITE
// -----------------------------------------------------------------------------
//
// pathIs - 'path' is 'base' or below it
//
static bool pathIs(const char* path, const char* base)
{
  size_t len = strlen(base);

  return (strncmp(path, base, len) == 0) && ((path[len] == 0) || (path[len] == '/'));
}



// -----------------------------------------------------------------------------
//
// snapshotWritable - an operation a Snapshot takes: the Core API's and the Temporal API's on
// Entities (§ 7.9.2) - not a Service invocation on one
//
static bool snapshotWritable(const char* path)
{
  if (path == NULL)
    return false;

  if (pathIs(path, "/ngsi-ld/v1/entities"))
    return (strstr(path, "/services") == NULL);

  return pathIs(path, "/ngsi-ld/v1/entityOperations") ||
         pathIs(path, "/ngsi-ld/v1/temporal/entities") ||
         pathIs(path, "/ngsi-ld/v1/temporal/entityOperations");
}
#endif



bool ldSnapshotWriteGuard(void)
{
  if (readSnapshotIdHeader() == NULL)
    return true;

  if (corRest.in.verb == CorVerbGet || corRest.in.verb == CorVerbHead)
    return true;

#if COR_FEATURE_SNAPSHOT_WRITE
  if (snapshotWritable(corRest.in.urlPath))
  {
    //
    // § 7.9.2: the operation is applied to the snapshot - its own tenant - and in a local scope (no
    // Context Source Registration is used). A snapshot tenant has no subscription or registration
    // caches, so nothing is forwarded and nothing notified.
    //
    bool                 seen  = false;
    LdSnapshotCacheItem* itemP = ldSnapshotItemFromHeader(&seen);

    if (itemP == NULL)
      return false;                                   // 404, set

    if (itemP->snapTenantP == NULL)
    {
      ldError(409, LD_ERROR_CONFLICT, "Conflict", "Snapshot '%s' has no information to update yet", itemP->id);
      return false;
    }

    corNgsild.tenantP = itemP->snapTenantP;
    corNgsild.local   = true;
    return true;
  }

  ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Operation Not Supported",
          "a Snapshot takes the operations on Entities and their Temporal Evolution - not this one");
#else
  ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Operation Not Supported",
          "NGSILD-Snapshot header cannot be combined with write operations or with subscription / registration creation; snapshots are immutable in NGSI-LD v1.9.1");
#endif
  return false;
}

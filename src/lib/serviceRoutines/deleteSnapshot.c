//
// FILE            deleteSnapshot.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// DELETE /ngsi-ld/v1/snapshots/{id} — Delete Snapshot (§ 5.16.5).
//
#include <stdbool.h>                                     // bool
#include <string.h>                                      // strrchr

#include "swRest/SwRestState.h"                          // swRest

#include "swNgsild/swNgsild.h"                           // ldError, swNgsild
#include "swNgsild/LdProblem.h"                          // LD_ERROR_*
#include "swNgsild/LdSnapshotCache.h"                    // LdSnapshotCache, ldSnapshotCacheItemDelete, ldSnapshotCacheItemLookup
#include "swNgsild/ldSnapshotNotify.h"                   // ldSnapshotNotify

#include "db/DbDriver.h"                                 // db
#include "db/Tenant.h"                                   // Tenant
#include "db/snapshotTenant.h"                           // snapshotTenantDestroy
#include "troe/TroeDriver.h"                             // troe

#include "serviceRoutines/deleteSnapshot.h"              // Own interface


bool deleteSnapshot(void)
{
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  const char* slash = strrchr(swRest.in.urlPath, '/');
  const char* id    = (slash != NULL) ? slash + 1 : swRest.in.urlPath;

  if (id == NULL || id[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Snapshot id missing in URL");
    return true;
  }

  if (tenantP->snapshotCacheP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "Snapshot '%s' not found", id);
    return true;
  }

  LdSnapshotCache*     cacheP = (LdSnapshotCache*) tenantP->snapshotCacheP;
  LdSnapshotCacheItem* itemP  = ldSnapshotCacheItemLookup(cacheP, id);
  if (itemP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "Snapshot '%s' not found", id);
    return true;
  }

  // Capture the snap-tenant pointer before the cache item is reclaimed —
  // ldSnapshotCacheItemDelete unlinks but doesn't free the cache-allocator
  // memory, so reading after delete would still work but is fragile.
  Tenant* snapTenantP = (Tenant*) itemP->snapTenantP;

  // § 5.16.6 — fire deletion notification (expiresAt forced to past)
  // BEFORE the cache item is unlinked.
  ldSnapshotNotify(itemP, true);

  ldSnapshotCacheItemDelete(cacheP, id);

  // Drop the persisted metadata + the entity store + the temporal
  // store + free the in-memory tenant struct. troe.tenantDrop runs
  // before db.tenantDrop so a TRoE plugin that needs a still-live
  // current-state tenant for its cleanup (rare) can rely on it.
  if (db.snapshotDelete != NULL)
    db.snapshotDelete(tenantP, id);
  if (troe.tenantDrop != NULL && snapTenantP != NULL)
    troe.tenantDrop(snapTenantP);
  if (db.tenantDrop != NULL && snapTenantP != NULL)
    db.tenantDrop(snapTenantP);
  snapshotTenantDestroy(snapTenantP);

  swRest.out.httpStatusCode = 204;
  return true;
}

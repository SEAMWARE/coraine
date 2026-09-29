//
// FILE            deleteSnapshot.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// DELETE /ngsi-ld/v1/snapshots/{id} — Delete Snapshot (§ 5.16.5).
//
#include <stdbool.h>                                     // bool
#include <string.h>                                      // strrchr

#include "corRest/CorRestState.h"                          // corRest

#include "corNgsild/corNgsild.h"                           // ldError, corNgsild
#include "corNgsild/LdProblem.h"                          // LD_ERROR_*
#include "corNgsild/LdSnapshotCache.h"                    // LdSnapshotCache, ldSnapshotCacheItemDelete, ldSnapshotCacheItemLookup
#include "corNgsild/ldSnapshotNotify.h"                   // ldSnapshotNotify

#include "db/DbDriver.h"                                 // db
#include "db/Tenant.h"                                   // Tenant
#include "db/snapshotTenant.h"                           // snapshotTenantDestroy
#include "troe/TroeDriver.h"                             // troe

#include "serviceRoutines/deleteSnapshot.h"              // Own interface


bool deleteSnapshot(void)
{
  Tenant* tenantP = (Tenant*) corNgsild.tenantP;

  const char* slash = strrchr(corRest.in.urlPath, '/');
  const char* id    = (slash != NULL) ? slash + 1 : corRest.in.urlPath;

  if (id == NULL || id[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Missing URL Component",
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
  LdSnapshotCacheItem* itemP  = ldSnapshotCacheItemLookupPinned(cacheP, id);
  if (itemP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "Snapshot '%s' not found", id);
    return true;
  }

  // § 5.16.6 — fire deletion notification (expiresAt forced to past)
  // BEFORE the cache item is unlinked (it is pinned).
  ldSnapshotNotify(itemP, true);

  //
  // Unlink it (the cache's reference goes) and drop the persisted metadata. The snapshot's own
  // stores - entities, temporal - and its tenant struct go with the LAST reference, in the
  // cache's destroy hook (snapshotItemDestroy): now, at our unpin below, unless the capture
  // worker or a read routed to this snapshot still has it. They were destroyed right here,
  // under such a worker or read - and twice by two DELETEs of the same snapshot.
  //
  ldSnapshotCacheItemDelete(cacheP, id);

  if (db.snapshotDelete != NULL)
    db.snapshotDelete(tenantP, id);

  ldSnapshotCacheItemUnpin(itemP);

  corRest.out.httpStatusCode = 204;
  return true;
}

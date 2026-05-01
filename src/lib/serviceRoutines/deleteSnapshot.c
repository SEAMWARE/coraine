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
#include "swNgsild/LdSnapshotCache.h"                    // LdSnapshotCache, ldSnapshotCacheItemDelete

#include "db/Tenant.h"                                   // Tenant

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

  if (tenantP->snapshotCacheP == NULL || !ldSnapshotCacheItemDelete(
        (LdSnapshotCache*) tenantP->snapshotCacheP, id))
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "Snapshot '%s' not found", id);
    return true;
  }

  swRest.out.httpStatusCode = 204;
  return true;
}

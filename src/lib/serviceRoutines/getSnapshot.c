//
// FILE            getSnapshot.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/snapshots/{id} — Retrieve Snapshot Status (§ 5.16.3).
//
#include <stdbool.h>                                     // bool
#include <string.h>                                      // strrchr

#include "swRest/SwRestState.h"                          // swRest
#include "kjson/kjLookup.h"                              // kjLookup
#include "kjson/kjBuilder.h"                             // kjChildRemove
#include "kjson/kjClone.h"                               // kjClone

#include "swNgsild/swNgsild.h"                           // ldError, swNgsild
#include "swNgsild/LdProblem.h"                          // LD_ERROR_*
#include "swNgsild/LdSnapshotCache.h"                    // LdSnapshotCache, ldSnapshotCacheItemLookup

#include "db/Tenant.h"                                   // Tenant

#include "serviceRoutines/getSnapshot.h"                 // Own interface


bool getSnapshot(void)
{
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  // /ngsi-ld/v1/snapshots/<id>  → take the last path segment.
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

  itemP->lastUsedAt = swRest.requestStartTime;

  // Clone into the per-request kalloc so swRest can render it. Strip
  // the hidden "_snapSeq" field used for boot reload — it's an
  // implementation detail not part of the public Snapshot data type.
  KjNode* clone = kjClone(swRest.kjsonP, itemP->tree);
  KjNode* seqP  = (clone != NULL) ? kjLookup(clone, "_snapSeq") : NULL;
  if (seqP != NULL)
    kjChildRemove(clone, seqP);

  swRest.out.responseTree   = clone;
  swRest.out.httpStatusCode = 200;
  return true;
}

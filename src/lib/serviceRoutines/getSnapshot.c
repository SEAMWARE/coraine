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

#include "corRest/CorRestState.h"                          // corRest
#include "kjson/kjLookup.h"                              // kjLookup
#include "kjson/kjBuilder.h"                             // kjChildRemove
#include "kjson/kjClone.h"                               // kjClone

#include "corNgsild/corNgsild.h"                           // ldError, corNgsild
#include "corNgsild/LdProblem.h"                          // LD_ERROR_*
#include "corNgsild/LdSnapshotCache.h"                    // LdSnapshotCache, ldSnapshotCacheItemLookup

#include "db/Tenant.h"                                   // Tenant

#include "serviceRoutines/getSnapshot.h"                 // Own interface


bool getSnapshot(void)
{
  Tenant* tenantP = (Tenant*) corNgsild.tenantP;

  // /ngsi-ld/v1/snapshots/<id>  → take the last path segment.
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
  LdSnapshotCacheItem* itemP  = ldSnapshotCacheItemLookup(cacheP, id);
  if (itemP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "Snapshot '%s' not found", id);
    return true;
  }

  itemP->lastUsedAt = corRest.requestStartTime;

  // Clone into the per-request kalloc so corRest can render it. Strip
  // the hidden "_snapSeq" field used for boot reload — it's an
  // implementation detail not part of the public Snapshot data type.
  KjNode* clone = kjClone(corRest.kjsonP, itemP->tree);
  KjNode* seqP  = (clone != NULL) ? kjLookup(clone, "_snapSeq") : NULL;
  if (seqP != NULL)
    kjChildRemove(clone, seqP);

  corRest.out.responseTree   = clone;
  corRest.out.httpStatusCode = 200;
  return true;
}

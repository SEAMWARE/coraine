//
// FILE            patchSnapshot.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// PATCH /ngsi-ld/v1/snapshots/{id} — Update Snapshot Status (§ 5.16.4).
//
// Mutable members per § 5.2.41: snapshotPriority, snapshotLifetime,
// endpoint, receiverInfo. Everything else (id, type, snapshotQueries,
// snapshotTemporalQueries, snapshotStatus, snapshotQueriesDetails,
// timestamps) is immutable in this operation.
//
// snapshotLifetime is parsed as an ISO 8601 duration (§ 5.2.41) and
// expiresAt is recomputed as createdAt + duration so the cleanup
// loop can act on the fresh deadline.
//
#include <stdbool.h>                                     // bool
#include <stdint.h>                                      // uint64_t
#include <stdio.h>                                       // snprintf
#include <string.h>                                      // strrchr, strcmp
#include <time.h>                                        // gmtime_r

#include "swRest/SwRestState.h"                          // swRest

#include "kalloc/kaAlloc.h"                              // kaAlloc
#include "kjson/KjNode.h"                                // KjNode
#include "kjson/kjLookup.h"                              // kjLookup
#include "kjson/kjBuilder.h"                             // kjString, kjInteger, kjChildAdd, kjChildReplace
#include "kjson/kjChildReplace.h"                        // kjChildReplace
#include "kjson/kjClone.h"                               // kjClone
#include "kjson/kjFree.h"                                // kjFree

#include "swNgsild/swNgsild.h"                           // ldError, swNgsild
#include "swNgsild/LdProblem.h"                          // LD_ERROR_*
#include "swNgsild/LdSnapshotCache.h"                    // LdSnapshotCache, ldSnapshotCacheItemLookup
#include "swNgsild/ldSnapshotNotify.h"                   // ldSnapshotNotify
#include "swNgsild/ldIso8601Duration.h"                  // ldIso8601DurationParseNs

#include "db/DbDriver.h"                                 // db
#include "db/Tenant.h"                                   // Tenant

#include "serviceRoutines/patchSnapshot.h"               // Own interface


//
// nsToIso - format a ns-epoch timestamp as ISO 8601 UTC (long-lived
// allocation in swRest.kalloc, since we hand it to the cache tree).
//
static char* nsToIso(uint64_t ns)
{
  time_t  s   = (time_t) (ns / 1000000000ULL);
  long    ms  = (long)   ((ns % 1000000000ULL) / 1000000ULL);
  struct tm tm;
  gmtime_r(&s, &tm);

  char* buf = (char*) kaAlloc(&swRest.kalloc, 80);
  snprintf(buf, 80, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
  return buf;
}



//
// IMMUTABLE_FIELDS - members the client must not include in a fragment.
//
static const char* IMMUTABLE_FIELDS[] = {
  "id", "type",
  "snapshotQueries", "snapshotTemporalQueries",
  "snapshotStatus",
  "snapshotQueriesDetails", "snapshotTemporalQueriesDetails",
  "createdAt", "modifiedAt", "expiresAt", "lastUsedAt",
  NULL
};



bool patchSnapshot(void)
{
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;
  KjNode* fragP   = swRest.in.requestTree;

  const char* slash = strrchr(swRest.in.urlPath, '/');
  const char* id    = (slash != NULL) ? slash + 1 : swRest.in.urlPath;

  if (id == NULL || id[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Missing URL Component",
            "Snapshot id missing in URL");
    return true;
  }

  if (fragP == NULL || fragP->type != KjObject)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Not a JSON Object",
            "Snapshot fragment must be a JSON object");
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

  // Reject immutable fields.
  for (int i = 0; IMMUTABLE_FIELDS[i] != NULL; i++)
  {
    if (kjLookup(fragP, IMMUTABLE_FIELDS[i]) != NULL)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Immutable Field",
              "field '%s' cannot be modified via PATCH", IMMUTABLE_FIELDS[i]);
      return true;
    }
  }

  // Validate snapshotPriority if present.
  KjNode* prioP = kjLookup(fragP, "snapshotPriority");
  if (prioP != NULL)
  {
    long pn = (prioP->type == KjInt)   ? (long) prioP->value.i
            : (prioP->type == KjFloat) ? (long) prioP->value.f
            : -1L;
    if (pn < 1 || pn > 10)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Field Value",
              "'snapshotPriority' must be an integer between 1 and 10");
      return true;
    }
  }

  // Validate snapshotLifetime (ISO 8601 duration) if present.
  KjNode* lifeP = kjLookup(fragP, "snapshotLifetime");
  int64_t lifeNs = -1;
  if (lifeP != NULL)
  {
    if (lifeP->type != KjString || (lifeNs = ldIso8601DurationParseNs(lifeP->value.s)) < 0)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Field Value",
              "'snapshotLifetime' must be a positive ISO 8601 duration (e.g. \"PT1H\", \"P1D\")");
      return true;
    }
  }

  // Apply each fragment field to the cached tree (cache allocator is
  // long-lived; we re-stamp by replacing the node when present, adding
  // it otherwise).
  for (KjNode* fP = fragP->value.firstChildP; fP != NULL; fP = fP->next)
  {
    if (fP->name == NULL) continue;

    KjNode* dest = kjLookup(itemP->tree, fP->name);
    KjNode* clone = kjClone(NULL, fP);                  // long-lived clone
    if (dest != NULL)
    {
      // itemP->tree is an all-malloc clone — kjChildReplace only swaps the
      // pointer, so free the replaced node to avoid orphaning it.
      kjChildReplace(itemP->tree, dest, clone);
      kjFree(dest);
    }
    else
      kjChildAdd(itemP->tree, clone);
  }

  // Refresh modifiedAt on the cached tree (live tree mutation only).
  itemP->modifiedAt = swRest.requestStartTime;
  if (prioP != NULL)
  {
    long pn = (prioP->type == KjInt) ? (long) prioP->value.i : (long) prioP->value.f;
    itemP->priority = (int) pn;
  }

  // Recompute expiresAt = createdAt + new lifetime, mirror to the cache
  // tree, and add it to the persisted fragment (so the DB sees the new
  // deadline even though clients can't set expiresAt directly).
  // The cache tree must hold long-lived strings (kjString with NULL
  // allocator → malloc-backed copy); the per-request fragment can use
  // swRest.kjsonP since the DB plugin renders & sends before request end.
  if (lifeNs > 0)
  {
    itemP->expiresAt = itemP->createdAt + (uint64_t) lifeNs;
    char*   expIso = nsToIso(itemP->expiresAt);
    KjNode* dest   = kjLookup(itemP->tree, "expiresAt");
    if (dest != NULL)
    {
      // all-malloc clone — kjChildRemove only unlinks, so free the old node.
      kjChildRemove(itemP->tree, dest);
      kjFree(dest);
    }
    kjChildAdd(itemP->tree, kjString(NULL, "expiresAt", expIso));
    kjChildAdd(fragP, kjString(swRest.kjsonP, "expiresAt", expIso));
  }

  // Persist the patch so it survives a restart. The DB plugin applies
  // JSON Merge Patch (null → unset; otherwise set/replace).
  if (db.snapshotUpdate != NULL)
    db.snapshotUpdate(tenantP, id, fragP);

  // § 5.16.6 — fire SnapshotNotification on any status update.
  ldSnapshotNotify(itemP, false);

  swRest.out.httpStatusCode = 204;
  return true;
}

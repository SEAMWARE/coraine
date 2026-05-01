//
// FILE            postSnapshot.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/snapshots — Create Snapshot (§ 5.16.1).
//
// Phase 1 scope (intentionally narrow):
//   - Validate the Snapshot doc per § 5.2.41.
//   - Generate id if absent, default snapshotPriority=5, set createdAt
//     /modifiedAt/expiresAt + status="success".
//   - Store in the per-tenant Snapshot cache.
//   - Return 201 Created with Location.
//
// Deferred to later phases:
//   - Async query execution against snapshotQueries / snapshotTemporalQueries.
//   - Storage of retrieved entities into a snapshot-tagged store.
//   - Status transitions (preparing → success/partial/empty/failure).
//   - SnapshotNotification on the endpoint URI.
//
#include <stdbool.h>                                     // bool
#include <stdint.h>                                      // uint64_t
#include <string.h>                                      // strlen, strcpy, strcat
#include <stdio.h>                                       // snprintf

#include "swRest/SwRestState.h"                          // swRest
#include "kalloc/kaAlloc.h"                              // kaAlloc
#include "kjson/KjNode.h"                                // KjNode
#include "kjson/kjLookup.h"                              // kjLookup
#include "kjson/kjBuilder.h"                             // kjString, kjChildAdd

#include "swNgsild/swNgsild.h"                           // ldError, swNgsild
#include "swNgsild/LdProblem.h"                          // LD_ERROR_*
#include "swNgsild/LdSnapshotCache.h"                    // LdSnapshotCache, ldSnapshotCacheItemAdd
#include "swRest/swRestOutHeader.h"                      // swRestOutHeaderAdd

#include "db/Tenant.h"                                   // Tenant

#include "serviceRoutines/postSnapshot.h"                // Own interface


// -----------------------------------------------------------------------------
//
// snapshotIdGenerate -
//
static char* snapshotIdGenerate(void)
{
  static int counter = 0;
  char* buf = (char*) kaAlloc(&swRest.kalloc, 64);
  snprintf(buf, 64, "urn:ngsi-ld:Snapshot:%lx:%04x",
           (long) (swRest.requestStartTime / 1000000000ULL), ++counter & 0xFFFF);
  return buf;
}



// -----------------------------------------------------------------------------
//
// validateSnapshot - § 5.2.41 cardinality / type checks.
//
// Phase 1: minimal — just ensure type=="Snapshot" and at least one of
// snapshotQueries / snapshotTemporalQueries is present. Full schema
// validation lands when async execution does.
//
static bool validateSnapshot(KjNode* snapP)
{
  if (snapP == NULL || snapP->type != KjObject)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Snapshot payload must be a JSON object");
    return false;
  }

  KjNode* typeP = kjLookup(snapP, "type");
  if (typeP == NULL || typeP->type != KjString || strcmp(typeP->value.s, "Snapshot") != 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Snapshot 'type' must be \"Snapshot\"");
    return false;
  }

  KjNode* qP  = kjLookup(snapP, "snapshotQueries");
  KjNode* tqP = kjLookup(snapP, "snapshotTemporalQueries");
  if (qP == NULL && tqP == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "at least one of 'snapshotQueries' or 'snapshotTemporalQueries' is required");
    return false;
  }

  if (qP != NULL && qP->type != KjArray)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "'snapshotQueries' must be an array");
    return false;
  }
  if (tqP != NULL && tqP->type != KjArray)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "'snapshotTemporalQueries' must be an array");
    return false;
  }

  KjNode* prioP = kjLookup(snapP, "snapshotPriority");
  if (prioP != NULL)
  {
    long pn = (prioP->type == KjInt)   ? (long) prioP->value.i
            : (prioP->type == KjFloat) ? (long) prioP->value.f
            : -1L;
    if (pn < 1 || pn > 10)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "'snapshotPriority' must be an integer between 1 and 10");
      return false;
    }
  }
  return true;
}



// -----------------------------------------------------------------------------
//
// nsToIso - format ns timestamp as ISO 8601 UTC.
//
static char* nsToIso(uint64_t ns)
{
  time_t  s   = (time_t) (ns / 1000000000ULL);
  long    ms  = (long)   ((ns % 1000000000ULL) / 1000000ULL);
  struct tm tm;
  gmtime_r(&s, &tm);

  char* buf = (char*) kaAlloc(&swRest.kalloc, 32);
  snprintf(buf, 32, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
  return buf;
}



// -----------------------------------------------------------------------------
//
// addStringIfAbsent / replaceString - small builders.
//
static void replaceString(KjNode* parent, const char* name, const char* value)
{
  KjNode* p = kjLookup(parent, name);
  if (p != NULL && p->type == KjString)
    p->value.s = (char*) value;
  else
    kjChildAdd(parent, kjString(swRest.kjsonP, name, (char*) value));
}



// -----------------------------------------------------------------------------
//
// postSnapshot -
//
bool postSnapshot(void)
{
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;
  KjNode* snapP   = swRest.in.requestTree;

  if (!validateSnapshot(snapP))
    return true;

  // Auto-generate id if absent.
  KjNode* idP = kjLookup(snapP, "id");
  if (idP == NULL)
  {
    char* gid = snapshotIdGenerate();
    kjChildAdd(snapP, kjString(swRest.kjsonP, "id", gid));
    idP = kjLookup(snapP, "id");
  }
  else if (idP->type != KjString)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "Snapshot 'id' must be a string");
    return true;
  }

  if (tenantP->snapshotCacheP == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "Snapshot cache not initialised");
    return true;
  }

  LdSnapshotCache* cacheP = (LdSnapshotCache*) tenantP->snapshotCacheP;

  if (ldSnapshotCacheItemLookup(cacheP, idP->value.s) != NULL)
  {
    ldError(409, LD_ERROR_ALREADY_EXISTS, "Already Exists",
            "Snapshot '%s' already exists", idP->value.s);
    return true;
  }

  // System-generated members. Phase 1 sets status=success directly —
  // there's no async execution yet so there's nothing to fail.
  uint64_t  now = swRest.requestStartTime;
  uint64_t  exp = now + 3600ULL * 1000000000ULL;  // default 1h

  replaceString(snapP, "createdAt",      nsToIso(now));
  replaceString(snapP, "modifiedAt",     nsToIso(now));
  replaceString(snapP, "expiresAt",      nsToIso(exp));
  replaceString(snapP, "lastUsedAt",     nsToIso(now));
  replaceString(snapP, "snapshotStatus", "success");

  // Default priority.
  if (kjLookup(snapP, "snapshotPriority") == NULL)
    kjChildAdd(snapP, kjInteger(swRest.kjsonP, "snapshotPriority", 5));

  if (ldSnapshotCacheItemAdd(cacheP, snapP) == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "snapshot cache add failed");
    return true;
  }

  // 201 Created + Location: /ngsi-ld/v1/snapshots/{id}
  swRest.out.httpStatusCode = 201;
  const char* prefix = "/ngsi-ld/v1/snapshots/";
  int locLen = strlen(prefix) + strlen(idP->value.s) + 1;
  char* locBuf = (char*) kaAlloc(&swRest.kalloc, locLen);
  strcpy(locBuf, prefix);
  strcat(locBuf, idP->value.s);
  swRestOutHeaderAdd("Location", locBuf);

  return true;
}

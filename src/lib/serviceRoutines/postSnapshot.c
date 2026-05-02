//
// FILE            postSnapshot.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/snapshots — Create Snapshot (§ 5.16.1).
//
// Phase 1: bare CRUD store (validation, id assignment, cache add).
// Phase 2a (this revision): synchronous execution of snapshotQueries
// against the local DB, freezing matched entities into the snapshot's
// own store via ldSnapshotExecQueries. Status transitions to one of
// success / partial / empty / failure based on per-query outcomes.
//
// Deferred to later phases:
//   - snapshotTemporalQueries (TRoE integration, Phase 4).
//   - Distributed snapshotQueries (forward to CSRs per § 5.7.2.4).
//   - Background execution (so the create call returns before queries
//     finish; § 5.16.1.4 says "executed in the background").
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
#include "kjson/kjBuilder.h"                             // kjString, kjInteger, kjChildAdd, kjObject
#include "kjson/kjClone.h"                               // kjClone

#include "db/DbDriver.h"                                 // db

#include "swNgsild/swNgsild.h"                           // ldError, swNgsild
#include "swNgsild/LdProblem.h"                          // LD_ERROR_*
#include "swNgsild/LdSnapshotCache.h"                    // LdSnapshotCache, ldSnapshotCacheItemAdd
#include "swNgsild/ldSnapshotNotify.h"                   // ldSnapshotNotify
#include "swNgsild/ldIso8601Duration.h"                  // ldIso8601DurationParseNs
#include "swRest/swRestOutHeader.h"                      // swRestOutHeaderAdd

#include "db/Tenant.h"                                   // Tenant
#include "db/snapshotTenant.h"                           // snapshotTenantCreate
#include "troe/TroeDriver.h"                             // troe

#include "serviceRoutines/ldSnapshotExec.h"              // ldSnapshotExecQueries
#include "serviceRoutines/ldSnapshotExecTemporal.h"      // ldSnapshotExecTemporalQueries
#include "serviceRoutines/ldSnapshotCaptureAsync.h"      // ldSnapshotCaptureAsync
#include "serviceRoutines/postSnapshot.h"                // Own interface


extern bool asyncSnapshot;  // swBroker.c CLI flag


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

  KjNode* lifeP = kjLookup(snapP, "snapshotLifetime");
  if (lifeP != NULL)
  {
    if (lifeP->type != KjString || ldIso8601DurationParseNs(lifeP->value.s) < 0)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "'snapshotLifetime' must be a positive ISO 8601 duration (e.g. \"PT1H\", \"P1D\")");
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

  // System-generated members. Status starts as "preparing" per
  // § 5.16.1.4; ldSnapshotExecQueries below transitions it to one of
  // success / partial / empty / failure once queries have run.
  uint64_t  now = swRest.requestStartTime;
  uint64_t  exp = now + 3600ULL * 1000000000ULL;  // default 1h

  // § 5.2.41 snapshotLifetime → expiresAt. Already validated above.
  KjNode* lifeP = kjLookup(snapP, "snapshotLifetime");
  if (lifeP != NULL && lifeP->type == KjString)
  {
    int64_t durNs = ldIso8601DurationParseNs(lifeP->value.s);
    if (durNs > 0)
      exp = now + (uint64_t) durNs;
  }

  replaceString(snapP, "createdAt",      nsToIso(now));
  replaceString(snapP, "modifiedAt",     nsToIso(now));
  replaceString(snapP, "expiresAt",      nsToIso(exp));
  replaceString(snapP, "lastUsedAt",     nsToIso(now));
  replaceString(snapP, "snapshotStatus", "preparing");

  // Default priority.
  if (kjLookup(snapP, "snapshotPriority") == NULL)
    kjChildAdd(snapP, kjInteger(swRest.kjsonP, "snapshotPriority", 5));

  LdSnapshotCacheItem* itemP = ldSnapshotCacheItemAdd(cacheP, snapP);
  if (itemP == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "snapshot cache add failed");
    return true;
  }

  // The snap-tenant holds the frozen entity bodies — see snapshotTenant.h.
  // Captured entities stream into it via db.entityCreate; reads route
  // through it via the standard entity stack.
  itemP->snapTenantP = snapshotTenantCreate(tenantP, itemP->snapSeq);
  if (itemP->snapTenantP == NULL)
  {
    ldSnapshotCacheItemDelete(cacheP, idP->value.s);
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "snapshot tenant setup failed");
    return true;
  }

  // § 5.16 — temporal capture lives on the snap-tenant's TRoE store.
  // Plugins that need per-tenant setup (timescale schemas, ...) hook
  // here; current plugins set tenantSetup=NULL and the call is a no-op.
  if (troe.tenantSetup != NULL)
    troe.tenantSetup((Tenant*) itemP->snapTenantP);

  // Persist the snapshot metadata BEFORE running the queries. The
  // initial status is "preparing" — async mode returns 201 right
  // afterwards and the worker thread updates the status when capture
  // completes; sync mode runs the capture inline and overwrites the
  // status before persistence (we still write twice — once here as
  // "preparing", once at the end with the final status — to keep the
  // crash-recovery contract identical for sync and async paths).
  if (db.snapshotCreate != NULL)
  {
    if (kjLookup(itemP->tree, "_snapSeq") == NULL)
      kjChildAdd(itemP->tree, kjInteger(NULL, "_snapSeq", itemP->snapSeq));
    db.snapshotCreate(tenantP, itemP->id, itemP->tree);
  }

  if (asyncSnapshot)
  {
    // § 5.16.1.4 — "the following is executed in the background".
    // Worker spawns, mutates itemP->tree + itemP->status, calls
    // db.snapshotUpdate, fires the notification. POST returns 201
    // immediately with snapshotStatus="preparing".
    ldSnapshotCaptureAsync(cacheP, itemP, tenantP,
                            swNgsild.splitEntitiesSet,
                            swNgsild.splitEntitiesVal);
  }
  else
  {
    // Inline (sync) capture path.
    ldSnapshotExecQueries(cacheP, itemP, tenantP);
    ldSnapshotExecTemporalQueries(cacheP, itemP, tenantP);

    // Re-persist with the final status + both detail arrays.
    if (db.snapshotUpdate != NULL && itemP->tree != NULL)
    {
      KjNode* fragment = kjObject(swRest.kjsonP, NULL);
      KjNode* sP       = kjLookup(itemP->tree, "snapshotStatus");
      if (sP != NULL && sP->type == KjString)
        kjChildAdd(fragment, kjString(swRest.kjsonP, "snapshotStatus", sP->value.s));
      KjNode* dP = kjLookup(itemP->tree, "snapshotQueriesDetails");
      if (dP != NULL)
        kjChildAdd(fragment, kjClone(swRest.kjsonP, dP));
      KjNode* tdP = kjLookup(itemP->tree, "snapshotTemporalQueriesDetails");
      if (tdP != NULL)
        kjChildAdd(fragment, kjClone(swRest.kjsonP, tdP));
      if (fragment->value.firstChildP != NULL)
        db.snapshotUpdate(tenantP, itemP->id, fragment);
    }

    // § 5.16.6 — fire SnapshotNotification after capture is fully done.
    ldSnapshotNotify(itemP, false);
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

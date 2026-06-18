//
// FILE            cloneSnapshot.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/snapshots/{id}/clone — Clone Snapshot (§ 5.16.2).
//
// The frozen entity store of the source snapshot is duplicated into a
// new cache item. createdAt/modifiedAt/lastUsedAt are reset to now,
// expiresAt resets to the default 1h horizon. Optional body fields
// (snapshotPriority, snapshotLifetime, endpoint, receiverInfo) are
// honoured; snapshotQueriesDetails / snapshotTemporalQueriesDetails
// must NOT be supplied (§ 5.16.2.3).
//
#include <stdbool.h>                                     // bool
#include <stdint.h>                                      // uint64_t
#include <string.h>                                      // strlen, strcpy, strcat, strrchr, strcmp
#include <stdio.h>                                       // snprintf
#include <time.h>                                        // gmtime_r

#include "swRest/SwRestState.h"                          // swRest
#include "swRest/swRestOutHeader.h"                      // swRestOutHeaderAdd

#include "kalloc/kaAlloc.h"                              // kaAlloc
#include "kjson/KjNode.h"                                // KjNode
#include "kjson/kjLookup.h"                              // kjLookup
#include "kjson/kjBuilder.h"                             // kjString, kjInteger, kjChildAdd, kjChildRemove
#include "kjson/kjChildReplace.h"                        // kjChildReplace
#include "kjson/kjClone.h"                               // kjClone

#include "swNgsild/swNgsild.h"                           // ldError, swNgsild
#include "swNgsild/LdProblem.h"                          // LD_ERROR_*
#include "swNgsild/LdSnapshotCache.h"                    // LdSnapshotCache, ldSnapshotCacheItemAdd
#include "swNgsild/ldSnapshotNotify.h"                   // ldSnapshotNotify

#include "db/DbDriver.h"                                 // db, DB_OK
#include "db/DbQueryFilter.h"                            // DbQueryFilter
#include "db/Tenant.h"                                   // Tenant
#include "db/snapshotTenant.h"                           // snapshotTenantCreate, snapshotTenantDestroy
#include "troe/TroeDriver.h"                             // troe

#include "serviceRoutines/cloneSnapshot.h"               // Own interface


//
// nsToIso - format ns timestamp as ISO 8601 UTC. Local copy because
// the postSnapshot helper is static.
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



static char* generateSnapshotId(void)
{
  static int counter = 0;
  char* buf = (char*) kaAlloc(&swRest.kalloc, 64);
  snprintf(buf, 64, "urn:ngsi-ld:Snapshot:%lx:%04x",
           (long) (swRest.requestStartTime / 1000000000ULL), ++counter & 0xFFFF);
  return buf;
}



static void replaceString(KjNode* parent, const char* name, const char* value)
{
  KjNode* p = kjLookup(parent, name);
  if (p != NULL && p->type == KjString)
    p->value.s = (char*) value;
  else
    kjChildAdd(parent, kjString(swRest.kjsonP, name, (char*) value));
}



bool cloneSnapshot(void)
{
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  // The route pattern is /ngsi-ld/v1/snapshots/<id>/clone — wildcard[0]
  // is the source snapshot id.
  const char* sourceId = swRest.in.wildcard[0];
  if (sourceId == NULL || sourceId[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Missing URL Component",
            "Source snapshot id missing in URL");
    return true;
  }

  if (tenantP->snapshotCacheP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "Snapshot '%s' not found", sourceId);
    return true;
  }

  LdSnapshotCache*     cacheP   = (LdSnapshotCache*) tenantP->snapshotCacheP;
  LdSnapshotCacheItem* sourceP  = ldSnapshotCacheItemLookup(cacheP, sourceId);
  if (sourceP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "Snapshot '%s' not found", sourceId);
    return true;
  }

  KjNode* bodyP = swRest.in.requestTree;
  if (bodyP != NULL && bodyP->type != KjObject)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Not a JSON Object",
            "Snapshot fragment must be a JSON object");
    return true;
  }

  // § 5.16.2.3 — request body must NOT carry the *Details arrays.
  if (bodyP != NULL)
  {
    if (kjLookup(bodyP, "snapshotQueriesDetails") != NULL ||
        kjLookup(bodyP, "snapshotTemporalQueriesDetails") != NULL)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Immutable Field",
              "snapshotQueriesDetails / snapshotTemporalQueriesDetails must not be supplied on clone");
      return true;
    }
  }

  // Determine new id: body's id, or auto-generate.
  const char* newId = NULL;
  if (bodyP != NULL)
  {
    KjNode* idP = kjLookup(bodyP, "id");
    if (idP != NULL && idP->type == KjString)
      newId = idP->value.s;
  }
  if (newId == NULL)
    newId = generateSnapshotId();

  if (ldSnapshotCacheItemLookup(cacheP, newId) != NULL)
  {
    ldError(409, LD_ERROR_ALREADY_EXISTS, "Already Exists",
            "Snapshot '%s' already exists", newId);
    return true;
  }

  //
  // Build the new snapshot tree as a deep clone of the source's tree,
  // then re-stamp identity and timestamps. snapshotQueries are inherited
  // from the source; the spec is silent on whether they are copied but
  // it makes the clone self-describing without introducing a "blank"
  // tree we'd then have to re-fetch from somewhere.
  //
  KjNode* newTree = kjClone(swRest.kjsonP, sourceP->tree);

  // Body overrides for mutable fields.
  if (bodyP != NULL)
  {
    KjNode* prioP = kjLookup(bodyP, "snapshotPriority");
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
      KjNode* dest = kjLookup(newTree, "snapshotPriority");
      if (dest != NULL) dest->value.i = pn;
      else              kjChildAdd(newTree, kjInteger(swRest.kjsonP, "snapshotPriority", pn));
    }

    const char* COPY_OVER[] = { "snapshotLifetime", "endpoint", "receiverInfo", NULL };
    for (int i = 0; COPY_OVER[i] != NULL; i++)
    {
      KjNode* fP = kjLookup(bodyP, COPY_OVER[i]);
      if (fP == NULL) continue;
      KjNode* clone = kjClone(swRest.kjsonP, fP);
      KjNode* dest  = kjLookup(newTree, COPY_OVER[i]);
      if (dest != NULL) kjChildReplace(newTree, dest, clone);
      else              kjChildAdd(newTree, clone);
    }
  }

  uint64_t now = swRest.requestStartTime;
  uint64_t exp = now + 3600ULL * 1000000000ULL;
  replaceString(newTree, "id",             (char*) newId);
  replaceString(newTree, "createdAt",      nsToIso(now));
  replaceString(newTree, "modifiedAt",     nsToIso(now));
  replaceString(newTree, "expiresAt",      nsToIso(exp));
  replaceString(newTree, "lastUsedAt",     nsToIso(now));
  // § 5.16.2.4 — clone enters as "preparing"; entity copy is
  // synchronous below so we transition to the source's status (or
  // failure if no entities to copy) before returning.
  replaceString(newTree, "snapshotStatus", "preparing");

  // Strip *Details from cloned tree — they belong to the source's
  // execution and would mislead readers of the clone.
  KjNode* d1 = kjLookup(newTree, "snapshotQueriesDetails");
  if (d1 != NULL) kjChildRemove(newTree, d1);
  KjNode* d2 = kjLookup(newTree, "snapshotTemporalQueriesDetails");
  if (d2 != NULL) kjChildRemove(newTree, d2);

  LdSnapshotCacheItem* newItemP = ldSnapshotCacheItemAdd(cacheP, newTree);
  if (newItemP == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "snapshot cache add failed");
    return true;
  }

  newItemP->snapTenantP = snapshotTenantCreate(tenantP, newItemP->snapSeq);
  if (newItemP->snapTenantP == NULL)
  {
    ldSnapshotCacheItemDelete(cacheP, newId);
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "snapshot tenant setup failed");
    return true;
  }

  if (troe.tenantSetup != NULL)
    troe.tenantSetup((Tenant*) newItemP->snapTenantP);

  // Copy frozen entities from source's snap-tenant to the clone's
  // snap-tenant. Streamed via db.entityQuery(source) → db.entityCreate(clone).
  // Phase #140 will swap the read for a cursor-based iterator so the
  // intermediate KjArray vanishes; for now the per-snapshot entity set
  // is bounded by what the source captured.
  int copied = 0;
  Tenant* sourceSnapP = (Tenant*) sourceP->snapTenantP;
  Tenant* cloneSnapP  = (Tenant*) newItemP->snapTenantP;
  if (sourceSnapP != NULL && cloneSnapP != NULL)
  {
    DbQueryFilter all = {0};
    KjNode* arrayP = NULL;
    if (db.entityQuery(sourceSnapP, &all, &arrayP) == DB_OK &&
        arrayP != NULL && arrayP->type == KjArray)
    {
      for (KjNode* eP = arrayP->value.firstChildP; eP != NULL; eP = eP->next)
      {
        KjNode* idP = kjLookup(eP, "id");
        if (idP == NULL) idP = kjLookup(eP, "_id");
        if (idP == NULL || idP->type != KjString) continue;

        if (db.entityCreate(cloneSnapP, idP->value.s, eP) == DB_OK)
          copied++;
      }
    }

    // Temporal-row copy. No time filter ⇒ all rows. Best-effort:
    // a TRoE plugin without entityTemporalQuery / Create just leaves
    // the clone's temporal store empty (matches the no-capture case).
    if (troe.entityTemporalQuery != NULL && troe.entityTemporalCreate != NULL)
    {
      TroeQueryFilter tqf;
      memset(&tqf, 0, sizeof(tqf));
      TroeRangeInfo   tRange;
      memset(&tRange, 0, sizeof(tRange));

      KjNode* tArr = NULL;
      if (troe.entityTemporalQuery(sourceSnapP, &tqf, &tArr, &tRange) == TROE_OK &&
          tArr != NULL && tArr->type == KjArray)
      {
        for (KjNode* eP = tArr->value.firstChildP; eP != NULL; eP = eP->next)
        {
          if (eP->type != KjObject) continue;
          troe.entityTemporalCreate(cloneSnapP, eP);
        }
      }
    }
  }

  // Final status. Mirror source's outcome family: if source had
  // entities, clone is "success"; if source was empty, clone is
  // "empty"; if source failed, clone fails.
  const char* finalStatus =
      (sourceP->status == LdSnapshotFailure) ? "failure" :
      (copied > 0)                            ? "success" :
      "empty";

  KjNode* sP = kjLookup(newItemP->tree, "snapshotStatus");
  if (sP != NULL && sP->type == KjString)
    sP->value.s = (char*) finalStatus;

  // Persist the clone's metadata so it survives restart. _snapSeq lets
  // the boot reload reconstruct the snap-tenant DB name.
  if (db.snapshotCreate != NULL)
  {
    if (kjLookup(newItemP->tree, "_snapSeq") == NULL)
      kjChildAdd(newItemP->tree, kjInteger(NULL, "_snapSeq", newItemP->snapSeq));
    db.snapshotCreate(tenantP, newItemP->id, newItemP->tree);
  }

  ldSnapshotNotify(newItemP, false);

  // 201 Created + Location.
  swRest.out.httpStatusCode = 201;
  const char* prefix = "/ngsi-ld/v1/snapshots/";
  int locLen = strlen(prefix) + strlen(newId) + 1;
  char* locBuf = (char*) kaAlloc(&swRest.kalloc, locLen);
  strcpy(locBuf, prefix);
  strcat(locBuf, newId);
  swRestOutHeaderAdd("Location", locBuf);

  return true;
}

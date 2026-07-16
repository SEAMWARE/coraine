//
// FILE            patchCsourceRegistration.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// PATCH /ngsi-ld/v1/csourceRegistrations/{registrationId}  (NGSI-LD § 5.9.3)
//
#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldCheckRegistration.h"            // ldCheckRegistration
#include "swNgsild/LdOp.h"                           // LdOpUpdateRegistration
#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem
#include "swNgsild/ldRegCache.h"                     // ldRegCacheItemLookup, ldRegCacheItemRemove, ldRegCacheItemAdd
#include "swNgsild/LdSubCache.h"                     // LdSubCache
#include "swNgsild/ldCsrSubNotify.h"                 // ldCsrSubMatchingSubIds, ldCsrSubOnRegUpdate
#include "swNgsild/ldRegSubMerge.h"                  // ldRegSubMerge
#include "swNgsild/ldSysTimestamp.h"                 // ldSysTimestampModify

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/regConflictCheck.h"        // regConflictCheck, regModeOf
#include "serviceRoutines/patchCsourceRegistration.h" // Own interface



// -----------------------------------------------------------------------------
//
// patchCsourceRegistration -
//
bool patchCsourceRegistration(void)
{
  const char* regId    = swRest.in.wildcard[0];
  KjNode*     fragment = swRest.in.requestTree;

  if (ldCheckRegistration(fragment, LdOpUpdateRegistration, /*merged*/false, &swRest.kalloc) == false)
    return true;

  if (db.registrationUpdate == NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented", "registration CRUD not supported by this DB plugin");
    return true;
  }

  //
  // § 5.11.7 — snapshot which CSR-subs match the CSR PRE-update. The
  // old cache item's pre-parsed fields are freed by ldRegCacheItemRemove
  // below, so the "was matching" set has to be captured now. Borrowed
  // subId pointers are stable for the sub cache's lifetime.
  //
  // The whole snapshot → DB update → cache refresh is one atomic unit under
  // the reg-cache WRITE lock: it serializes concurrent CSR CRUD (which would
  // otherwise corrupt the shared list / free an item a reader holds) and
  // excludes the match-path readers. Held across the DB round-trips so the
  // cache always reflects the DB write that this PATCH performed.
  //
  Tenant*     tenantP   = (Tenant*) swNgsild.tenantP;
  LdRegCache* regCacheP = (LdRegCache*) tenantP->regCacheP;
  char**      wasMatchingIds = NULL;

  ldRegCacheWrLock(regCacheP);

  if (regCacheP != NULL && tenantP->regSubCacheP != NULL)
  {
    LdRegCacheItem* oldItemP = ldRegCacheItemLookup(regCacheP, regId);
    if (oldItemP != NULL)
      wasMatchingIds = ldCsrSubMatchingSubIds((LdSubCache*) tenantP->regSubCacheP, oldItemP, &swRest.kalloc);
  }

  //
  // The broker owns the merge (§ 5.9.3 / TS 104-175 clause-8). Load the current
  // registration, apply the update fragment here — JSON Merge Patch, with
  // urn:ngsi-ld:null members (already resolved to KjNull by the validator)
  // deleting their target — and hand the DB plugin a complete document to store.
  // Keeping the NGSI-LD merge/delete semantics in the broker means every DB
  // plugin is a dumb store and none of them re-implement (and drift on) it.
  //
  KjNode* mergedRegP = NULL;
  int     rr         = (db.registrationRetrieve != NULL) ? db.registrationRetrieve(tenantP, regId, &mergedRegP) : DB_NOT_FOUND;

  if (rr == DB_NOT_FOUND || mergedRegP == NULL)
  {
    ldRegCacheUnlock(regCacheP);
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "registration '%s' not found", regId);
    return true;
  }

  if (rr != DB_OK)
  {
    ldRegCacheUnlock(regCacheP);
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error retrieving registration '%s'", regId);
    return true;
  }

  ldRegSubMerge(mergedRegP, fragment, swRest.kjsonP);

  //
  // § 5.9.3 — re-validate the COMPLETE merged result before persisting. The
  // fragment validator only sees the members the PATCH carried; cross-member
  // invariants — auxiliary mode vs write operations, exclusive mode vs the
  // information[] structure (§ 9.3.3), a TimeInterval whose startAt/endAt
  // straddle the deep-merge — only become visible in the merged document.
  // Running the Create-grade validator in 'merged' mode enforces the full
  // mandatory/consistency shape while tolerating a stored-and-elapsed expiresAt
  // and the server-owned fields the stored registration carries.
  //
  if (ldCheckRegistration(mergedRegP, LdOpCreateRegistration, /*merged*/true, &swRest.kalloc) == false)
  {
    ldRegCacheUnlock(regCacheP);
    return true;  // ldCheckRegistration already raised the 400
  }

  //
  // § 12.2.3.4 — the merge may have turned the registration exclusive/redirect
  // or widened its entity/attribute coverage. Re-run the same mode-conflict
  // check the create path enforces, skipping this registration's own (still
  // cached, pre-update) entry: a PATCH must not install a registration that a
  // direct create would have rejected with 409.
  //
  if (regConflictCheck(mergedRegP, regModeOf(mergedRegP), regId, &swRest.kalloc))
  {
    ldRegCacheUnlock(regCacheP);
    return true;  // regConflictCheck already raised the 409 ldError
  }

  // § 6.4.5 — bump modifiedAt to now; createdAt (from the retrieved tree) stays
  ldSysTimestampModify(mergedRegP);

  int r = db.registrationUpdate(tenantP, regId, mergedRegP);

  if (r != DB_OK)
  {
    ldRegCacheUnlock(regCacheP);
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error updating registration '%s'", regId);
    return true;
  }

  // Refresh the cache from the merged document we just stored — no second
  // retrieve. Safe under the wrlock: no reader can be walking the list and no
  // other writer can be mid-mutation, so the transient remove→add is exclusive.
  LdRegCacheItem* newItemP = NULL;
  if (regCacheP != NULL)
  {
    ldRegCacheItemRemove(regCacheP, regId);
    newItemP = ldRegCacheItemAdd(regCacheP, mergedRegP, &swRest.kalloc);
  }

  //
  // Fan out updated / newlyMatching / noLongerMatching notifications.
  //
  if (newItemP != NULL && tenantP->regSubCacheP != NULL)
    ldCsrSubOnRegUpdate((LdSubCache*) tenantP->regSubCacheP, newItemP, wasMatchingIds);

  ldRegCacheUnlock(regCacheP);

  swRest.out.httpStatusCode = 204;
  return true;
}

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

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/patchCsourceRegistration.h" // Own interface



// -----------------------------------------------------------------------------
//
// patchCsourceRegistration -
//
bool patchCsourceRegistration(void)
{
  const char* regId    = swRest.in.wildcard[0];
  KjNode*     fragment = swRest.in.requestTree;

  if (ldCheckRegistration(fragment, LdOpUpdateRegistration, &swRest.kalloc) == false)
    return true;

  if (db.registrationUpdate == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented", "registration CRUD not supported by this DB plugin");
    return true;
  }

  //
  // § 5.11.7 — snapshot which CSR-subs match the CSR PRE-update. The
  // old cache item's pre-parsed fields are freed by ldRegCacheItemRemove
  // below, so the "was matching" set has to be captured now. Borrowed
  // subId pointers are stable for the sub cache's lifetime.
  //
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;
  char**  wasMatchingIds = NULL;
  if (tenantP->regCacheP != NULL && tenantP->regSubCacheP != NULL)
  {
    LdRegCacheItem* oldItemP = ldRegCacheItemLookup((LdRegCache*) tenantP->regCacheP, regId);
    if (oldItemP != NULL)
      wasMatchingIds = ldCsrSubMatchingSubIds((LdSubCache*) tenantP->regSubCacheP, oldItemP, &swRest.kalloc);
  }

  int r = db.registrationUpdate(tenantP, regId, fragment);

  if (r == DB_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "registration '%s' not found", regId);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error updating registration '%s'", regId);
    return true;
  }

  // Refresh the cache: remove old item, re-add merged tree from DB
  LdRegCacheItem* newItemP = NULL;
  if (tenantP->regCacheP != NULL)
  {
    ldRegCacheItemRemove((LdRegCache*) tenantP->regCacheP, regId);

    KjNode* updatedRegP = NULL;
    if (db.registrationRetrieve != NULL && db.registrationRetrieve(tenantP, regId, &updatedRegP) == DB_OK && updatedRegP != NULL)
      newItemP = ldRegCacheItemAdd((LdRegCache*) tenantP->regCacheP, updatedRegP);
  }

  //
  // Fan out updated / newlyMatching / noLongerMatching notifications.
  //
  if (newItemP != NULL && tenantP->regSubCacheP != NULL)
    ldCsrSubOnRegUpdate((LdSubCache*) tenantP->regSubCacheP, newItemP, wasMatchingIds);

  swRest.out.httpStatusCode = 204;
  return true;
}

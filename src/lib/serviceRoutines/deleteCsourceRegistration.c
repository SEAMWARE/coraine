//
// FILE            deleteCsourceRegistration.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// DELETE /ngsi-ld/v1/csourceRegistrations/{registrationId}  (NGSI-LD § 5.9.4)
//
#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem
#include "swNgsild/ldRegCache.h"                     // ldRegCacheItemRemove, ldRegCacheItemLookup
#include "swNgsild/LdSubCache.h"                     // LdSubCache
#include "swNgsild/ldCsrSubNotify.h"                 // ldCsrSubOnRegDelete
#include "swNgsild/ldDistSub.h"                      // ldDistSubOnRegDelete
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "kjson/KjNode.h"                              // KjNode

#include "serviceRoutines/deleteCsourceRegistration.h" // Own interface



//
// distSubPersist - persist subordinate mapping after on-reg-delete cleanup
//
static void distSubPersist(LdSubCacheItem* itemP, void* userData)
{
  if (itemP == NULL || itemP->subId == NULL || db.subscriptionUpdate == NULL)
    return;

  Tenant* tP    = (Tenant*) userData;
  KjNode* fragP = ldDistSubSubordinatesFragment(itemP, swRest.kjsonP);
  if (fragP == NULL)
    return;

  db.subscriptionUpdate(tP, itemP->subId, fragP);
}



// -----------------------------------------------------------------------------
//
// deleteCsourceRegistration -
//
bool deleteCsourceRegistration(void)
{
  const char* regId = swRest.in.wildcard[0];

  if (db.registrationDelete == NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented", "registration CRUD not supported by this DB plugin");
    return true;
  }

  int r = db.registrationDelete((Tenant*) swNgsild.tenantP, regId);

  if (r == DB_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "registration '%s' not found", regId);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error deleting registration '%s'", regId);
    return true;
  }

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  //
  // § 5.11.7 — fan out "noLongerMatching" CsourceNotifications to
  // any CSR-sub that matched the CSR being deleted. Must happen
  // BEFORE the cache remove because the matcher walks the reg item's
  // pre-parsed fields, which ldRegCacheItemRemove frees.
  //
  // wrlock held across the lookup → fanout → remove so the item being deleted
  // can't be freed/moved by a concurrent CSR write while the fanout reads its
  // pre-parsed fields, and the list mutation is exclusive of match-path readers.
  LdRegCache* regCacheP = (LdRegCache*) tenantP->regCacheP;
  ldRegCacheWrLock(regCacheP);
  if (regCacheP != NULL)
  {
    LdRegCacheItem* regItemP = ldRegCacheItemLookup(regCacheP, regId);
    if (regItemP != NULL && tenantP->regSubCacheP != NULL)
      ldCsrSubOnRegDelete((LdSubCache*) tenantP->regSubCacheP, regItemP);

    // § 5.8.1.4 — drop any subordinate (derived) subs that were forwarded
    // to this CSR. Best-effort remote DELETE is sent before unlinking the
    // local mapping. Must run before ldRegCacheItemRemove.
    if (regItemP != NULL && tenantP->subCacheP != NULL)
    {
      const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);
      ldDistSubOnRegDelete((LdSubCache*) tenantP->subCacheP, regItemP, ownAlias,
                           distSubPersist, tenantP);
    }

    ldRegCacheItemRemove(regCacheP, regId);
  }
  ldRegCacheUnlock(regCacheP);

  swRest.out.httpStatusCode = 204;
  return true;
}

//
// FILE            deleteCsourceSubscription.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// DELETE /ngsi-ld/v1/csourceSubscriptions/{id}  (NGSI-LD § 5.11.6)
//
#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest

#include "swNgsild/swNgsild.h"                       // ldError, swNgsild
#include "swNgsild/LdSubCache.h"                     // LdSubCache
#include "swNgsild/ldSubCache.h"                     // ldSubCacheItemRemove, ldSubCacheItemLookup

#include "db/DbDriver.h"                             // db, DB_OK
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/deleteCsourceSubscription.h" // Own interface



bool deleteCsourceSubscription(void)
{
  const char* subId = swRest.in.wildcard[0];

  Tenant*     tenantP = (Tenant*) swNgsild.tenantP;
  LdSubCache* cacheP  = (LdSubCache*) tenantP->regSubCacheP;

  // No reg fanout here — the existence-check Lookup + Remove are a single
  // read-modify-write on the CSR-sub cache, so hold the wrlock across both.
  ldSubCacheWrLock(cacheP);
  if (cacheP == NULL || ldSubCacheItemLookup(cacheP, subId) == NULL)
  {
    ldSubCacheUnlock(cacheP);
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "CSR subscription '%s' not found", subId);
    return true;
  }

  ldSubCacheItemRemove(cacheP, subId);
  ldSubCacheUnlock(cacheP);

  if (db.subscriptionDelete != NULL)
    db.subscriptionDelete(tenantP, subId);

  swRest.out.httpStatusCode = 204;
  return true;
}

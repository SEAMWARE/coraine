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
  LdSubCache* cacheP  = (tenantP != NULL) ? (LdSubCache*) tenantP->regSubCacheP : NULL;

  if (cacheP == NULL || ldSubCacheItemLookup(cacheP, subId) == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "CSR subscription '%s' not found", subId);
    return true;
  }

  ldSubCacheItemRemove(cacheP, subId);

  if (db.subscriptionDelete != NULL)
    db.subscriptionDelete(tenantP, subId);

  swRest.out.httpStatusCode = 204;
  return true;
}

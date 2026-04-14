//
// FILE            deleteSubscription.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdSubCache.h"                     // LdSubCache
#include "swNgsild/ldSubCache.h"                     // ldSubCacheItemRemove

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/deleteSubscription.h"      // Own interface



// -----------------------------------------------------------------------------
//
// deleteSubscription -
//
bool deleteSubscription(void)
{
  const char* subId = swRest.in.wildcard[0];

  if (db.subscriptionDelete == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented", "subscription CRUD not supported by this DB plugin");
    return true;
  }

  int r = db.subscriptionDelete((Tenant*) swNgsild.tenantP, subId);

  if (r == DB_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "subscription '%s' not found", subId);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error deleting subscription '%s'", subId);
    return true;
  }

  //
  // Remove from subscription cache
  //
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;
  if (tenantP->subCacheP != NULL)
    ldSubCacheItemRemove((LdSubCache*) tenantP->subCacheP, subId);

  swRest.out.httpStatusCode = 204;
  return true;
}

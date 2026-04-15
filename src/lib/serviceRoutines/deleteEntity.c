//
// FILE            deleteEntity.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdSubCache.h"                     // LdSubCache
#include "swNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityDelete
#include "swNgsild/ldNotifyDefer.h"                  // ldNotifyDefer

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/deleteEntity.h"            // Own interface



// -----------------------------------------------------------------------------
//
// deleteEntity -
//
bool deleteEntity(void)
{
  const char* entityId = swRest.in.wildcard[0];

  //
  // Retrieve entity before deletion (needed for subscription notification payload)
  //
  KjNode* entityP = NULL;

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;
  if (tenantP->subCacheP != NULL)
    db.entityRetrieve(tenantP, entityId, &entityP);

  int r = db.entityDelete(tenantP, entityId);

  if (r == DB_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity '%s' not found", entityId);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error deleting entity '%s'", entityId);
    return true;
  }

  //
  // Subscription matching + notification (entity was retrieved before deletion)
  //
  if (tenantP->subCacheP != NULL && entityP != NULL)
    ldNotifyDefer((LdSubCache*) tenantP->subCacheP, entityP, LdNotifyEntityDelete, NULL);

  swRest.out.httpStatusCode = 204;
  return true;
}

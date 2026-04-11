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

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND

#include "serviceRoutines/deleteEntity.h"            // Own interface



// -----------------------------------------------------------------------------
//
// deleteEntity -
//
bool deleteEntity(void)
{
  const char* entityId = swRest.in.wildcard[0];

  int r = db.entityDelete((Tenant*) swNgsild.tenantP, entityId);

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

  swRest.out.httpStatusCode = 204;
  return true;
}

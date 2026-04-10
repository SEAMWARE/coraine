//
// FILE            getEntity.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldPickOmit

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND

#include "serviceRoutines/getEntity.h"               // Own interface



// -----------------------------------------------------------------------------
//
// getEntity -
//
bool getEntity(void)
{
  if (!dbEnabled)
  {
    ldError(501, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented", "no database plugin loaded");
    return true;
  }

  const char* entityId = swRest.in.wildcard[0];

  KjNode* entityP = NULL;
  int     r       = db.entityRetrieve((Tenant*) swNgsild.tenantP, entityId, &entityP);

  if (r == DB_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity '%s' not found", entityId);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error retrieving entity '%s'", entityId);
    return true;
  }

  // Apply pick/omit attribute projection
  if (swNgsild.pickV != NULL || swNgsild.omitV != NULL)
    ldPickOmit(entityP, swNgsild.pickV, swNgsild.omitV);

  swRest.out.responseTree = entityP;
  return true;
}

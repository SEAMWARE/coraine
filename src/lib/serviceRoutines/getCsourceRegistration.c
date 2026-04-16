//
// FILE            getCsourceRegistration.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/csourceRegistrations/{registrationId}  (NGSI-LD § 5.10.1)
//

#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldContextResolve

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getCsourceRegistration.h"  // Own interface



// -----------------------------------------------------------------------------
//
// getCsourceRegistration -
//
bool getCsourceRegistration(void)
{
  const char* regId = swRest.in.wildcard[0];

  if (db.registrationRetrieve == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented", "registration CRUD not supported by this DB plugin");
    return true;
  }

  KjNode* regP = NULL;
  int     r    = db.registrationRetrieve((Tenant*) swNgsild.tenantP, regId, &regP);

  if (r == DB_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "registration '%s' not found", regId);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error retrieving registration '%s'", regId);
    return true;
  }

  ldContextResolve();

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = regP;
  return true;
}

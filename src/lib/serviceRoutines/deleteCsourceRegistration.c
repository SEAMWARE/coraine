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
#include "swNgsild/LdRegCache.h"                     // LdRegCache
#include "swNgsild/ldRegCache.h"                     // ldRegCacheItemRemove

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/deleteCsourceRegistration.h" // Own interface



// -----------------------------------------------------------------------------
//
// deleteCsourceRegistration -
//
bool deleteCsourceRegistration(void)
{
  const char* regId = swRest.in.wildcard[0];

  if (db.registrationDelete == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented", "registration CRUD not supported by this DB plugin");
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
  if (tenantP->regCacheP != NULL)
    ldRegCacheItemRemove((LdRegCache*) tenantP->regCacheP, regId);

  swRest.out.httpStatusCode = 204;
  return true;
}

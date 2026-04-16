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
#include "swNgsild/LdRegCache.h"                     // LdRegCache
#include "swNgsild/ldRegCache.h"                     // ldRegCacheItemRemove, ldRegCacheItemAdd

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/patchCsourceRegistration.h" // Own interface



// -----------------------------------------------------------------------------
//
// patchCsourceRegistration -
//
bool patchCsourceRegistration(void)
{
  if (swNgsild.contextError)
    return true;

  const char* regId    = swRest.in.wildcard[0];
  KjNode*     fragment = swRest.in.requestTree;

  if (swRest.in.payload != NULL && fragment == NULL)
  {
    ldError(415, LD_ERROR_INVALID_REQUEST, "Unsupported Media Type",
            "supported Content-Types: application/json, application/ld+json");
    return true;
  }

  if (fragment == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "no payload");
    return true;
  }

  if (ldCheckRegistration(fragment, LdOpUpdateRegistration, &swRest.kalloc) == false)
    return true;

  if (db.registrationUpdate == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented", "registration CRUD not supported by this DB plugin");
    return true;
  }

  int r = db.registrationUpdate((Tenant*) swNgsild.tenantP, regId, fragment);

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
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;
  if (tenantP->regCacheP != NULL)
  {
    ldRegCacheItemRemove((LdRegCache*) tenantP->regCacheP, regId);

    KjNode* updatedRegP = NULL;
    if (db.registrationRetrieve != NULL && db.registrationRetrieve(tenantP, regId, &updatedRegP) == DB_OK && updatedRegP != NULL)
      ldRegCacheItemAdd((LdRegCache*) tenantP->regCacheP, updatedRegP);
  }

  swRest.out.httpStatusCode = 204;
  return true;
}

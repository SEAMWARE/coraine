//
// FILE            getCsourceRegistrations.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/csourceRegistrations  (NGSI-LD § 5.10.2 / § 6.8.3.2)
//
// First-cut behaviour: list all registrations for the tenant, paginated
// via limit/offset. The full discovery filter (type / attrs / q / geoQ /
// scopeQ / csf / temporal / id / idPattern) is delegated to a future
// matcher pass — for now URL params are parsed and accepted but the
// matching itself is not yet wired up.
//
#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldContextResolve

#include "db/DbDriver.h"                             // db, DB_OK
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getCsourceRegistrations.h" // Own interface



// -----------------------------------------------------------------------------
//
// getCsourceRegistrations -
//
bool getCsourceRegistrations(void)
{
  if (db.registrationQuery == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented", "registration CRUD not supported by this DB plugin");
    return true;
  }

  KjNode* arrayP = NULL;
  int     r      = db.registrationQuery((Tenant*) swNgsild.tenantP, swNgsild.limit, swNgsild.offset, &arrayP);

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error querying registrations");
    return true;
  }

  ldContextResolve();

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = arrayP;
  return true;
}

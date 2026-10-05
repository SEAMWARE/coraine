//
// FILE            deleteServiceRegistration.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// DELETE /ngsi-ld/v1/serviceRegistrations/{id}
//
#include "corRest/CorRestState.h"                     // corRest
#include "corNgsild/CorNgsild.h"                      // corNgsild
#include "corNgsild/ldError.h"                        // ldError
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*

#include "db/DbDriver.h"                              // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                                // Tenant
#include "serviceRoutines/deleteServiceRegistration.h"  // Own interface



// -----------------------------------------------------------------------------
//
// deleteServiceRegistration -
//
bool deleteServiceRegistration(void)
{
  const char* regId = corRest.in.wildcard[0];
  int         r     = (db.docDelete != NULL) ? db.docDelete((Tenant*) corNgsild.tenantP, "serviceRegistrations", regId) : DB_NOT_FOUND;

  if (r == DB_NOT_FOUND)
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "Service Registration '%s' not found", regId);
  else if (r != DB_OK)
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error deleting Service Registration '%s'", regId);
  else
    corRest.out.httpStatusCode = 204;

  return true;
}

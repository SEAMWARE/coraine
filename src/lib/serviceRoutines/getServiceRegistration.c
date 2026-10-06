//
// FILE            getServiceRegistration.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/serviceRegistrations/{id} - Retrieve Entity Service (GR CIM-055 § 6.4.12)
//
#include "corRest/CorRestState.h"                     // corRest
#include "corTree/CorNode.h"                          // CorNode
#include "corNgsild/CorNgsild.h"                      // corNgsild, ldContextResolve
#include "corNgsild/ldError.h"                        // ldError
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*

#include "db/DbDriver.h"                              // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                                // Tenant
#include "serviceExecution/seRegistrationRender.h"    // seRegistrationRender
#include "serviceRoutines/getServiceRegistration.h"   // Own interface



// -----------------------------------------------------------------------------
//
// getServiceRegistration -
//
bool getServiceRegistration(void)
{
  const char* regId = corRest.in.wildcard[0];
  CorNode*    regP  = NULL;
  int         r     = (db.docRetrieve != NULL) ? db.docRetrieve((Tenant*) corNgsild.tenantP, "serviceRegistrations", regId, &regP) : DB_NOT_FOUND;

  if (r == DB_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "Service Registration '%s' not found", regId);
    return true;
  }
  else if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error retrieving Service Registration '%s'", regId);
    return true;
  }

  ldContextResolve();
  seRegistrationRender(regP);

  corNgsild.rawResponse    = true;
  corRest.out.responseTree = regP;
  return true;
}

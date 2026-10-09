//
// FILE            postServiceRegistration.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// POST /ngsi-ld/v1/serviceRegistrations - Register Entity Service (GR CIM-055 § 6.4.11)
//
#include <stdio.h>                                    // snprintf
#include <string.h>                                   // strlen

#include "corRest/CorRestState.h"                     // corRest
#include "corRest/corRestOutHeader.h"                 // corRestOutHeaderAdd
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corTree/corTreeBuilder.h"                   // corTreeString, corTreeChildAdd
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corNgsild/CorNgsild.h"                      // corNgsild
#include "corNgsild/ldError.h"                        // ldError
#include "corJsonld/corLdCompact.h"                   // corLdCompact
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*
#include "corNgsild/ldIdGenerate.h"                   // ldIdGenerate
#include "corNgsild/ldSysTimestamp.h"                 // ldSysTimestampCreate

#include "db/DbDriver.h"                              // db, DB_OK, DB_ALREADY_EXISTS
#include "db/Tenant.h"                                // Tenant
#include "serviceExecution/seRegistrationCheck.h"     // seRegistrationCheck
#include "serviceExecution/seRegistrationMatch.h"     // seRegistrationNameConflict
#include "serviceRoutines/subscriptionQExpand.h"      // subscriptionQExpand
#include "serviceRoutines/postServiceRegistration.h"  // Own interface



// -----------------------------------------------------------------------------
//
// postServiceRegistration -
//
bool postServiceRegistration(void)
{
  CorNode* regP = corRest.in.requestTree;

  if (seRegistrationCheck(regP) == false)
    return true;

  CorNode* idP = corTreeLookup(regP, "id");

  if (idP == NULL)
  {
    idP = corTreeString(corRest.kallocP, "id", ldIdGenerate(&corRest.kalloc, "ServiceRegistration"));
    corTreeChildAdd(regP, idP);
  }

  if (db.docCreate == NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Operation Not Supported", "the database plugin keeps no Service Registrations");
    return true;
  }

  subscriptionQExpand(regP);                          // q stored with its attribute names expanded, as a subscription's

  //
  // A service name is unique within an entity: no other registration may offer it on an entity this one could select
  //
  const char* otherId = seRegistrationNameConflict(regP);

  if (otherId != NULL)
  {
    ldError(409, LD_ERROR_ALREADY_EXISTS, "Already Exists", "Service Registration '%s' already offers the service '%s' on entities this registration could select - a service name is unique within an entity",
            otherId, corLdCompact(corNgsild.contextP, corTreeLookup(corTreeLookup(regP, "serviceInformation"), "serviceName")->value.s));
    return true;
  }

  ldSysTimestampCreate(regP);

  int r = db.docCreate((Tenant*) corNgsild.tenantP, "serviceRegistrations", idP->value.s, regP);

  if (r == DB_ALREADY_EXISTS)
  {
    ldError(409, LD_ERROR_ALREADY_EXISTS, "Already Exists", "Service Registration '%s' already exists", idP->value.s);
    return true;
  }
  else if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error creating Service Registration '%s'", idP->value.s);
    return true;
  }

  int   size     = 64 + strlen(idP->value.s);
  char* location = (char*) corAlloc(&corRest.kalloc, size);

  snprintf(location, size, "/ngsi-ld/v1/serviceRegistrations/%s", idP->value.s);
  corRestOutHeaderAdd("Location", location);

  corRest.out.httpStatusCode = 201;
  return true;
}

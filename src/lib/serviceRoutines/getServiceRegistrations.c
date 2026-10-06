//
// FILE            getServiceRegistrations.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/serviceRegistrations - every Service Registration, or Discover Entity Services
// (GR CIM-055 § 6.4.13): with ?type (and ?id), those that apply to that entity - its type, its id
// against the registration's id / idPattern, and, when the entity exists, the registration's q and
// geoQ against it.
//
#include "corRest/CorRestState.h"                     // corRest
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeArray, corTreeChildAdd, corTreeChildRemove
#include "corNgsild/CorNgsild.h"                      // corNgsild, ldContextResolve
#include "corNgsild/ldError.h"                        // ldError
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*

#include "db/DbDriver.h"                              // db, DB_OK
#include "db/Tenant.h"                                // Tenant
#include "serviceExecution/seRegistrationMatch.h"     // seRegistrationMatches
#include "serviceExecution/seRegistrationRender.h"    // seRegistrationRender
#include "serviceRoutines/getServiceRegistrations.h"  // Own interface



// -----------------------------------------------------------------------------
//
// getServiceRegistrations -
//
bool getServiceRegistrations(void)
{
  Tenant*  tenantP = (Tenant*) corNgsild.tenantP;
  CorNode* regsP   = NULL;

  ldContextResolve();

  if (db.docQuery == NULL)
  {
    corNgsild.rawResponse    = true;
    corRest.out.responseTree = corTreeArray(corRest.kallocP, NULL);
    return true;
  }

  if (db.docQuery(tenantP, "serviceRegistrations", &regsP) != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error listing the Service Registrations");
    return true;
  }

  char**      typeV    = ((corNgsild.typeV != NULL) && (corNgsild.typeV[0] != NULL)) ? corNgsild.typeV : NULL;
  const char* entityId = ((corNgsild.idV != NULL) && (corNgsild.idV[0] != NULL)) ? corNgsild.idV[0] : NULL;
  CorNode*    entityP  = NULL;

  if ((entityId != NULL) && (db.entityRetrieve != NULL) && (db.entityRetrieve(tenantP, entityId, &entityP) != DB_OK))
    entityP = NULL;                                   // not (yet) an entity: its registrations by id and type only

  CorNode* outP = corTreeArray(corRest.kallocP, NULL);
  CorNode* regP = regsP->value.head;

  while (regP != NULL)
  {
    CorNode* nextP = regP->next;

    if ((typeV == NULL) && (entityId == NULL))
      ;                                               // every registration
    else if (seRegistrationMatches(regP, entityId, typeV, entityP) == false)
    {
      regP = nextP;
      continue;
    }

    corTreeChildRemove(regsP, regP);
    seRegistrationRender(regP);
    corTreeChildAdd(outP, regP);
    regP = nextP;
  }

  corNgsild.rawResponse    = true;
  corRest.out.responseTree = outP;
  return true;
}

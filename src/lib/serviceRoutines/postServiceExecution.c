//
// FILE            postServiceExecution.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// POST /ngsi-ld/v1/services - Create Service Execution (GR CIM-055 § 6.4.7.2), a simple one:
//   { "type": "ServiceExecution", "entityId", "serviceName", "executionInput"?, "notification"? }
// The broker sets the rest - its status above all.
//
#include <string.h>                                   // strcmp

#include "corRest/CorRestState.h"                     // corRest
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corNgsild/CorNgsild.h"                      // corNgsild, ldContextResolve
#include "corNgsild/ldError.h"                        // ldError
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*
#include "corJsonld/corLdCompact.h"                   // corLdCompact

#include "serviceExecution/seExecution.h"             // seExecute
#include "serviceExecution/seCombined.h"              // seCombinedCreate
#include "serviceRoutines/postServiceExecution.h"     // Own interface



// -----------------------------------------------------------------------------
//
// postServiceExecution -
//
bool postServiceExecution(void)
{
  CorNode* bodyP         = corRest.in.requestTree;
  CorNode* typeP         = NULL;
  CorNode* entityIdP     = NULL;
  CorNode* serviceNameP  = NULL;
  CorNode* inputP        = NULL;
  CorNode* notificationP = NULL;

  if ((bodyP == NULL) || (bodyP->type != CorObject))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "a Service Execution is a JSON object");
    return true;
  }

  //
  // Combined and Grouped Service Executions (and those made of a template) - seCombined.c
  //
  CorNode* bodyTypeP = corTreeLookup(bodyP, "type");

  if ((bodyTypeP != NULL) && (bodyTypeP->type == CorString) &&
      ((strcmp(bodyTypeP->value.s, "CombinedServiceExecution") == 0) || (strcmp(bodyTypeP->value.s, "GroupedServiceExecution") == 0)))
    return seCombinedCreate(bodyP);

  for (CorNode* mP = bodyP->value.head; mP != NULL; mP = mP->next)
  {
    if      (strcmp(mP->name, "type")           == 0) typeP         = mP;
    else if (strcmp(mP->name, "entityId")       == 0) entityIdP     = mP;
    else if (strcmp(mP->name, "serviceName")    == 0) serviceNameP  = mP;
    else if (strcmp(mP->name, "executionInput") == 0) inputP        = mP;
    else if (strcmp(mP->name, "notification")   == 0) notificationP = mP;
    else if (strcmp(mP->name, "executionStatus") == 0)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'executionStatus' is the broker's to set");
      return true;
    }
    else
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'%s' is not a member of a Service Execution", corLdCompact(corNgsild.contextP, mP->name));
      return true;
    }
  }

  if ((typeP == NULL) || (typeP->type != CorString) || (strcmp(typeP->value.s, "ServiceExecution") != 0))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "a Service Execution's 'type' is ServiceExecution, CombinedServiceExecution or GroupedServiceExecution");
    return true;
  }

  if ((entityIdP == NULL) || (entityIdP->type != CorString) || (serviceNameP == NULL) || (serviceNameP->type != CorString))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "a Service Execution names its 'entityId' and its 'serviceName'");
    return true;
  }

  if ((inputP != NULL) && (inputP->type != CorObject))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'executionInput' is a JSON object");
    return true;
  }

  if ((notificationP != NULL) && ((notificationP->type != CorObject) || (corTreeLookup(corTreeLookup(notificationP, "endpoint"), "uri") == NULL)))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'notification' needs an endpoint with a uri");
    return true;
  }

  return seExecute(SeCreate, entityIdP->value.s, serviceNameP->value.s, inputP, notificationP);
}

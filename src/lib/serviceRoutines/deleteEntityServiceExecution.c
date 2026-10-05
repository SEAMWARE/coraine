//
// FILE            deleteEntityServiceExecution.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// DELETE /ngsi-ld/v1/entities/{entityId}/services/{serviceName}/{executionId} - Cancel Entity Service
// Execution (GR CIM-055 § 6.4.6)
//
#include <string.h>                                   // strcmp

#include "corRest/CorRestState.h"                     // corRest
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corNgsild/CorNgsild.h"                      // corNgsild, ldContextResolve
#include "corNgsild/ldError.h"                        // ldError
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*
#include "corJsonld/corLdExpand.h"                    // corLdExpand

#include "serviceExecution/seExecution.h"             // seExecutionRetrieve, seExecutionCancel
#include "serviceRoutines/deleteEntityServiceExecution.h"  // Own interface



// -----------------------------------------------------------------------------
//
// deleteEntityServiceExecution -
//
bool deleteEntityServiceExecution(void)
{
  ldContextResolve();

  CorNode* execP = seExecutionRetrieve(corRest.in.wildcard[2]);

  if (execP == NULL)
    return true;

  CorNode*    entityIdP    = corTreeLookup(execP, "entityId");
  CorNode*    serviceNameP = corTreeLookup(execP, "serviceName");
  const char* serviceName  = corLdExpand(corNgsild.contextP, corRest.in.wildcard[1], &corRest.kalloc, NULL, NULL);

  if ((entityIdP == NULL) || (strcmp(entityIdP->value.s, corRest.in.wildcard[0]) != 0) ||
      (serviceNameP == NULL) || (strcmp(serviceNameP->value.s, serviceName) != 0))
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "Service Execution '%s' is not one of that service on that entity", corRest.in.wildcard[2]);
    return true;
  }

  return seExecutionCancel(execP);
}

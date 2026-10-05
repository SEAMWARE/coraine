//
// FILE            patchServiceExecution.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// PATCH /ngsi-ld/v1/services/{executionId} - Update Service Execution (GR CIM-055 § 6.4.9): the
// executor's report. Only with the Service-Execution header naming the execution.
//
#include <string.h>                                   // strcmp

#include "corRest/CorRestState.h"                     // corRest
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corNgsild/CorNgsild.h"                      // corNgsild, ldContextResolve
#include "corNgsild/ldError.h"                        // ldError
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*

#include "serviceExecution/seExecution.h"             // seExecutionRetrieve, seExecutionUpdate
#include "serviceExecution/seRequest.h"               // seRequestHeader
#include "serviceRoutines/patchServiceExecution.h"    // Own interface



// -----------------------------------------------------------------------------
//
// patchServiceExecution -
//
bool patchServiceExecution(void)
{
  const char* execId = corRest.in.wildcard[0];
  const char* header = seRequestHeader("Service-Execution");
  CorNode*    bodyP  = corRest.in.requestTree;

  if ((header == NULL) || (strcmp(header, execId) != 0))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "an executor's update names the execution in a 'Service-Execution' header");
    return true;
  }

  if ((bodyP == NULL) || (bodyP->type != CorObject) || (bodyP->value.head == NULL))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "the update of a Service Execution is a non-empty JSON object");
    return true;
  }

  CorNode* execP = seExecutionRetrieve(execId);

  if (execP == NULL)
    return true;

  return seExecutionUpdate(execP, bodyP);
}

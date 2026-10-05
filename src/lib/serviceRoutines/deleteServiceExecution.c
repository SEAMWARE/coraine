//
// FILE            deleteServiceExecution.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// DELETE /ngsi-ld/v1/services/{executionId} - Cancel/Delete Service Execution (GR CIM-055 § 6.4.10):
// a running execution is cancelled, a finished one deleted
//
#include "corRest/CorRestState.h"                     // corRest
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corNgsild/CorNgsild.h"                      // corNgsild, ldContextResolve
#include "corNgsild/ldError.h"                        // ldError
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*

#include "serviceExecution/seExecution.h"             // seExecutionRetrieve, seExecutionCancel
#include "serviceRoutines/deleteServiceExecution.h"   // Own interface



// -----------------------------------------------------------------------------
//
// deleteServiceExecution -
//
bool deleteServiceExecution(void)
{
  ldContextResolve();

  CorNode* execP = seExecutionRetrieve(corRest.in.wildcard[0]);

  if (execP == NULL)
    return true;

  return seExecutionCancel(execP);
}

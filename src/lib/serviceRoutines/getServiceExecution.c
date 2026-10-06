//
// FILE            getServiceExecution.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/services/{executionId} - Retrieve Service Execution Status (GR CIM-055 § 6.4.8)
//
#include "corRest/CorRestState.h"                     // corRest
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corNgsild/CorNgsild.h"                      // corNgsild, ldContextResolve
#include "corNgsild/ldError.h"                        // ldError
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*

#include "serviceExecution/seExecution.h"             // seExecutionRetrieve, seExecutionRender
#include "serviceRoutines/getServiceExecution.h"      // Own interface



// -----------------------------------------------------------------------------
//
// getServiceExecution -
//
bool getServiceExecution(void)
{
  ldContextResolve();

  CorNode* execP = seExecutionRetrieve(corRest.in.wildcard[0]);

  if (execP == NULL)
    return true;

  seExecutionRenderAll(execP);

  corNgsild.rawResponse    = true;
  corRest.out.responseTree = execP;
  return true;
}

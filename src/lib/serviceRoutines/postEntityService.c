//
// FILE            postEntityService.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// POST /ngsi-ld/v1/entities/{entityId}/services/{serviceName} - Invoke Entity Service (GR CIM-055 § 6.4.4)
//
// The body is the service's input, as the client wrote it (LdOpInvokeService: never expanded). The
// service name in the path is expanded with the request's @context, as an attribute name is.
//
#include "corRest/CorRestState.h"                     // corRest
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corNgsild/CorNgsild.h"                      // corNgsild, ldContextResolve
#include "corNgsild/ldError.h"                        // ldError
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*
#include "corJsonld/corLdExpand.h"                    // corLdExpand

#include "serviceExecution/seExecution.h"             // seExecute
#include "serviceRoutines/postEntityService.h"        // Own interface



// -----------------------------------------------------------------------------
//
// postEntityService -
//
bool postEntityService(void)
{
  CorNode* inputP = corRest.in.requestTree;

  if ((inputP != NULL) && (inputP->type != CorObject))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "a service's input is a JSON object");
    return true;
  }

  ldContextResolve();

  const char* serviceName = corLdExpand(corNgsild.contextP, corRest.in.wildcard[1], &corRest.kalloc, NULL, NULL);

  return seExecute(SeInvoke, corRest.in.wildcard[0], serviceName, inputP, NULL);
}

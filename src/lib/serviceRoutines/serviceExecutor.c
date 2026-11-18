//
// FILE            serviceExecutor.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// This broker as ANOTHER broker's Service Executor - the executor side of the requests a broker sends an
// HTTP executor (doc/service-execution.md, "An HTTP executor"; "Another broker as the executor"):
//
//   POST   /ngsi-ld/ex/v1/executor/{serviceName}[?reportTo=<the caller>]           Service-Execution: {id}
//          { "id": <entity id>, "type": <entity type>, <the input's members> }
//   DELETE /ngsi-ld/ex/v1/executor/{serviceName}/{id}[?reportTo=...]              - the cancel
//
// The other broker's registration has the endpoint http(s)://<this broker>/ngsi-ld/ex/v1/executor/{name}
// (or cor://), and ?reportTo=<its own address> when the service is asynchronous. The service is executed
// here as an invocation of {serviceName} on the entity, with this broker's own registration; the execution
// takes the caller's id. Synchronous: 200, the output. Asynchronous: 202, then every change reported to the
// caller with PATCH <reportTo>/ngsi-ld/v1/services/{id} - as an HTTP executor reports.
//
#include <string.h>                                   // strcmp

#include "corRest/CorRestState.h"                     // corRest
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeBuilder.h"               // corTreeChildRemove
#include "corNgsild/CorNgsild.h"                      // corNgsild, ldContextResolve
#include "corNgsild/ldError.h"                        // ldError
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*
#include "corNgsild/ldCheckUri.h"                     // ldUriValid
#include "corJsonld/corLdExpand.h"                    // corLdExpand

#include "serviceExecution/seExecution.h"             // seExecuteFor, seExecutionRetrieve, seExecutionCancel
#include "serviceExecution/seRequest.h"               // seRequestHeader, seRequestParam
#include "serviceRoutines/serviceExecutor.h"          // Own interface



// -----------------------------------------------------------------------------
//
// postServiceExecutor -
//
bool postServiceExecutor(void)
{
  const char* callerId = seRequestHeader("Service-Execution");
  const char* reportTo = seRequestParam("reportTo");
  CorNode*    bodyP    = corRest.in.requestTree;
  CorNode*    idP      = ((bodyP != NULL) && (bodyP->type == CorObject)) ? corTreeLookup(bodyP, "id") : NULL;

  if ((callerId == NULL) || (callerId[0] == 0))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "an execution handed over names itself in a 'Service-Execution' header");
    return true;
  }

  if ((idP == NULL) || (idP->type != CorString))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "an execution handed over is a JSON object with the entity's 'id' and the input");
    return true;
  }

  if ((reportTo != NULL) && ((ldUriValid(reportTo) == false) || ((strncmp(reportTo, "http://", 7) != 0) && (strncmp(reportTo, "https://", 8) != 0) && (strncmp(reportTo, "cor://", 6) != 0))))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'reportTo' is the address of the broker that asks: http(s):// or cor://");
    return true;
  }

  //
  // The input: the body without the entity's id and type
  //
  CorNode* inputP = corTreeClone(corRest.kallocP, bodyP);
  CorNode* mP     = inputP->value.head;

  while (mP != NULL)
  {
    CorNode* nextP = mP->next;

    if ((strcmp(mP->name, "id") == 0) || (strcmp(mP->name, "type") == 0))
      corTreeChildRemove(inputP, mP);

    mP = nextP;
  }

  ldContextResolve();

  const char* serviceName = corLdExpand(corNgsild.contextP, corRest.in.wildcard[0], &corRest.kalloc, NULL, NULL);

  return seExecuteFor(idP->value.s, serviceName, inputP, callerId, reportTo);
}



// -----------------------------------------------------------------------------
//
// deleteServiceExecutor -
//
bool deleteServiceExecutor(void)
{
  CorNode* execP = seExecutionRetrieve(corRest.in.wildcard[1]);

  if (execP == NULL)
    return true;                                      // seExecutionRetrieve: the 404

  return seExecutionCancel(execP);
}

//
// FILE            getServiceExecutions.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/services - the Service Executions, filtered by ?entityId, ?serviceName (expanded with
// the request's @context), ?executionStatus. Not in the report (§ 8.1.2 lists it as missing).
//
#include <string.h>                                   // strcmp

#include "corRest/CorRestState.h"                     // corRest
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corNgsild/CorNgsild.h"                      // corNgsild, ldContextResolve
#include "corNgsild/ldError.h"                        // ldError
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*
#include "corTree/corTreeBuilder.h"                   // corTreeArray, corTreeChildAdd, corTreeChildRemove
#include "corJsonld/corLdExpand.h"                    // corLdExpand

#include "db/DbDriver.h"                              // db, DB_OK
#include "db/Tenant.h"                                // Tenant
#include "serviceExecution/seExecution.h"             // seExecutionRender
#include "serviceExecution/seRequest.h"               // seRequestParam
#include "serviceRoutines/getServiceExecutions.h"     // Own interface



// -----------------------------------------------------------------------------
//
// is - does the execution's member 'name' equal 'value' (NULL: no filter)?
//
static bool is(CorNode* execP, const char* name, const char* value)
{
  if (value == NULL)
    return true;

  CorNode* mP = corTreeLookup(execP, name);

  return (mP != NULL) && (mP->type == CorString) && (strcmp(mP->value.s, value) == 0);
}



// -----------------------------------------------------------------------------
//
// getServiceExecutions -
//
bool getServiceExecutions(void)
{
  CorNode* execsP = NULL;

  ldContextResolve();

  if (db.docQuery == NULL)
  {
    corNgsild.rawResponse    = true;
    corRest.out.responseTree = corTreeArray(corRest.kallocP, NULL);
    return true;
  }

  if (db.docQuery((Tenant*) corNgsild.tenantP, "serviceExecutions", &execsP) != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error listing the Service Executions");
    return true;
  }

  const char* entityId    = seRequestParam("entityId");
  const char* serviceName = seRequestParam("serviceName");
  const char* status      = seRequestParam("executionStatus");

  if (serviceName != NULL)
    serviceName = corLdExpand(corNgsild.contextP, serviceName, &corRest.kalloc, NULL, NULL);

  CorNode* outP  = corTreeArray(corRest.kallocP, NULL);
  CorNode* execP = execsP->value.head;

  while (execP != NULL)
  {
    CorNode* nextP = execP->next;

    if (is(execP, "entityId", entityId) && is(execP, "serviceName", serviceName) && is(execP, "executionStatus", status))
    {
      corTreeChildRemove(execsP, execP);
      seExecutionRenderAll(execP);
      corTreeChildAdd(outP, execP);
    }

    execP = nextP;
  }

  corNgsild.rawResponse    = true;
  corRest.out.responseTree = outP;
  return true;
}

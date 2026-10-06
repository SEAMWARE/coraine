//
// FILE            patchCombinedServiceTemplate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// PATCH /ngsi-ld/v1/combinedServiceTemplate/{id} - Update Combined Service Template (GR CIM-055 § 6.4.16):
// each member of the fragment replaces the stored one (or is added); the result checked whole
//
#include <string.h>                                   // strcmp, strlen
#include <stdio.h>                                    // snprintf

#include "corRest/CorRestState.h"                     // corRest
#include "corRest/corRestOutHeader.h"                 // corRestOutHeaderAdd
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corTree/corTreeBuilder.h"                   // corTreeString, corTreeChildAdd, corTreeChildRemove
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corNgsild/CorNgsild.h"                      // corNgsild, ldContextResolve
#include "corNgsild/ldError.h"                        // ldError
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*
#include "corNgsild/ldIdGenerate.h"                   // ldIdGenerate
#include "corNgsild/ldSysTimestamp.h"                 // ldSysTimestampCreate, ldSysTimestampModify
#include "corNgsild/ldStripSysAttrs.h"                // ldStripSysAttrs

#include "db/DbDriver.h"                              // db, DB_*
#include "db/Tenant.h"                                // Tenant
#include "serviceExecution/seCombined.h"              // seTemplateCheck
#include "serviceRoutines/patchCombinedServiceTemplate.h"  // Own interface



// -----------------------------------------------------------------------------
//
// patchCombinedServiceTemplate -
//
bool patchCombinedServiceTemplate(void)
{
  const char* id        = corRest.in.wildcard[0];
  Tenant*     tenantP   = (Tenant*) corNgsild.tenantP;
  CorNode*    fragmentP = corRest.in.requestTree;
  CorNode*    templateP = NULL;

  if ((fragmentP == NULL) || (fragmentP->type != CorObject) || (fragmentP->value.head == NULL))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "the update of a Combined Service Template is a non-empty JSON object");
    return true;
  }

  int r = (db.docRetrieve != NULL) ? db.docRetrieve(tenantP, "serviceTemplates", id, &templateP) : DB_NOT_FOUND;

  if (r == DB_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "Combined Service Template '%s' not found", id);
    return true;
  }
  else if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error retrieving Combined Service Template '%s'", id);
    return true;
  }

  CorNode* mP = fragmentP->value.head;

  while (mP != NULL)
  {
    CorNode* nextP = mP->next;

    if ((strcmp(mP->name, "id") == 0) || (strcmp(mP->name, "type") == 0) || (strcmp(mP->name, "createdAt") == 0) || (strcmp(mP->name, "modifiedAt") == 0))
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'%s' of a Combined Service Template cannot be changed", mP->name);
      return true;
    }

    CorNode* oldP = corTreeLookup(templateP, mP->name);

    if (oldP != NULL)
      corTreeChildRemove(templateP, oldP);

    corTreeChildRemove(fragmentP, mP);
    corTreeChildAdd(templateP, mP);
    mP = nextP;
  }

  if (seTemplateCheck(templateP) == false)
    return true;

  ldSysTimestampModify(templateP);

  if (db.docReplace(tenantP, "serviceTemplates", id, templateP) != DB_OK)
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error updating Combined Service Template '%s'", id);
  else
    corRest.out.httpStatusCode = 204;

  return true;
}

//
// FILE            postCombinedServiceTemplate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// POST /ngsi-ld/v1/combinedServiceTemplate - Create Combined Service Template (GR CIM-055 § 6.4.14)
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
#include "serviceRoutines/postCombinedServiceTemplate.h"  // Own interface



// -----------------------------------------------------------------------------
//
// postCombinedServiceTemplate -
//
bool postCombinedServiceTemplate(void)
{
  CorNode* templateP = corRest.in.requestTree;

  if (seTemplateCheck(templateP) == false)
    return true;

  CorNode* idP = corTreeLookup(templateP, "id");

  if (idP == NULL)
  {
    idP = corTreeString(corRest.kallocP, "id", ldIdGenerate(&corRest.kalloc, "CombinedServiceTemplate"));
    corTreeChildAdd(templateP, idP);
  }
  else if ((idP->type != CorString) || (idP->value.s[0] == 0))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'id' must be a URI");
    return true;
  }

  if (db.docCreate == NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Operation Not Supported", "the database plugin keeps no Combined Service Templates");
    return true;
  }

  ldSysTimestampCreate(templateP);

  int r = db.docCreate((Tenant*) corNgsild.tenantP, "serviceTemplates", idP->value.s, templateP);

  if (r == DB_ALREADY_EXISTS)
  {
    ldError(409, LD_ERROR_ALREADY_EXISTS, "Already Exists", "Combined Service Template '%s' already exists", idP->value.s);
    return true;
  }
  else if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error creating Combined Service Template '%s'", idP->value.s);
    return true;
  }

  int   size     = 64 + strlen(idP->value.s);
  char* location = (char*) corAlloc(&corRest.kalloc, size);

  snprintf(location, size, "/ngsi-ld/v1/combinedServiceTemplate/%s", idP->value.s);
  corRestOutHeaderAdd("Location", location);
  corRest.out.httpStatusCode = 201;
  return true;
}

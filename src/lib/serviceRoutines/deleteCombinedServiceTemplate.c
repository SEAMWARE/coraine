//
// FILE            deleteCombinedServiceTemplate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// DELETE /ngsi-ld/v1/combinedServiceTemplate/{id} - Delete Combined Service Template (GR CIM-055 § 6.4.17)
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
#include "serviceRoutines/deleteCombinedServiceTemplate.h"  // Own interface



// -----------------------------------------------------------------------------
//
// deleteCombinedServiceTemplate -
//
bool deleteCombinedServiceTemplate(void)
{
  const char* id = corRest.in.wildcard[0];
  int         r  = (db.docDelete != NULL) ? db.docDelete((Tenant*) corNgsild.tenantP, "serviceTemplates", id) : DB_NOT_FOUND;

  if (r == DB_NOT_FOUND)
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "Combined Service Template '%s' not found", id);
  else if (r != DB_OK)
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error deleting Combined Service Template '%s'", id);
  else
    corRest.out.httpStatusCode = 204;

  return true;
}

//
// FILE            patchServiceRegistration.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// PATCH /ngsi-ld/v1/serviceRegistrations/{id} - each member of the fragment replaces the stored one
// (or is added); the merged registration is checked whole, and stored whole.
//
#include <string.h>                                   // strcmp

#include "corRest/CorRestState.h"                     // corRest
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corTree/corTreeBuilder.h"                   // corTreeChildAdd, corTreeChildRemove
#include "corNgsild/CorNgsild.h"                      // corNgsild
#include "corNgsild/ldError.h"                        // ldError
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*
#include "corNgsild/ldSysTimestamp.h"                 // ldSysTimestampModify

#include "db/DbDriver.h"                              // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                                // Tenant
#include "serviceExecution/seRegistrationCheck.h"     // seRegistrationCheck
#include "serviceRoutines/subscriptionQExpand.h"      // subscriptionQExpand
#include "serviceRoutines/patchServiceRegistration.h" // Own interface



// -----------------------------------------------------------------------------
//
// patchServiceRegistration -
//
bool patchServiceRegistration(void)
{
  const char* regId     = corRest.in.wildcard[0];
  Tenant*     tenantP   = (Tenant*) corNgsild.tenantP;
  CorNode*    fragmentP = corRest.in.requestTree;
  CorNode*    regP      = NULL;

  if ((fragmentP == NULL) || (fragmentP->type != CorObject) || (fragmentP->value.head == NULL))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "the update of a Service Registration is a non-empty JSON object");
    return true;
  }

  int r = (db.docRetrieve != NULL) ? db.docRetrieve(tenantP, "serviceRegistrations", regId, &regP) : DB_NOT_FOUND;

  if (r == DB_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "Service Registration '%s' not found", regId);
    return true;
  }
  else if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error retrieving Service Registration '%s'", regId);
    return true;
  }

  subscriptionQExpand(fragmentP);                     // a new q, expanded as the stored one is

  CorNode* mP = fragmentP->value.head;

  while (mP != NULL)
  {
    CorNode* nextP = mP->next;

    if ((strcmp(mP->name, "id") == 0) || (strcmp(mP->name, "createdAt") == 0) || (strcmp(mP->name, "modifiedAt") == 0))
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'%s' of a Service Registration cannot be changed", mP->name);
      return true;
    }

    CorNode* oldP = corTreeLookup(regP, mP->name);

    if (oldP != NULL)
      corTreeChildRemove(regP, oldP);

    corTreeChildRemove(fragmentP, mP);
    corTreeChildAdd(regP, mP);
    mP = nextP;
  }

  if (seRegistrationCheck(regP) == false)
    return true;

  ldSysTimestampModify(regP);

  r = db.docReplace(tenantP, "serviceRegistrations", regId, regP);

  if (r == DB_NOT_FOUND)
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "Service Registration '%s' not found", regId);
  else if (r != DB_OK)
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error updating Service Registration '%s'", regId);
  else
    corRest.out.httpStatusCode = 204;

  return true;
}

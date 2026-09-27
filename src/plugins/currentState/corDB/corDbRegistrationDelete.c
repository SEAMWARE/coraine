//
// FILE            corDbRegistrationDelete.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                   // strcmp

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeChildRemove
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corTree/corTreeFree.h"                      // corTreeFree
#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, Tenant
#include "currentState/corDB/corDbStore.h"          // corDbRegistrations
#include "currentState/corDB/corDbRegistrationDelete.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbRegistrationDelete -
//
int corDbRegistrationDelete(Tenant* tenantP, const char* regId)
{
  COR_DB_WRITE(tenantP);

  CorNode* registrations = corDbRegistrations(tenantP);

  for (CorNode* rP = registrations->value.head; rP != NULL; rP = rP->next)
  {
    CorNode* idP = corTreeLookup(rP, "id");

    if (idP != NULL && idP->type == CorString && strcmp(idP->value.s, regId) == 0)
    {
      corTreeChildRemove(registrations, rP);
      corTreeFree(rP);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

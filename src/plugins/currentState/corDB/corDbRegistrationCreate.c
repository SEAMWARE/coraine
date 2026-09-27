//
// FILE            corDbRegistrationCreate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                   // strcmp

#include "ktrace/kTrace.h"                            // KT_E
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeBuilder.h"                   // corTreeChildAdd
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "db/DbDriver.h"                              // DB_OK, DB_ALREADY_EXISTS, DB_ERR, Tenant
#include "currentState/corDB/corDbStore.h"          // corDbRegistrations
#include "currentState/corDB/corDbRegistrationCreate.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbRegistrationCreate -
//
int corDbRegistrationCreate(Tenant* tenantP, const char* regId, CorNode* regP)
{
  COR_DB_WRITE(tenantP);

  CorNode* registrations = corDbRegistrations(tenantP);

  for (CorNode* rP = registrations->value.firstChildP; rP != NULL; rP = rP->next)
  {
    CorNode* idP = corTreeLookup(rP, "id");

    if (idP != NULL && idP->type == CorString && strcmp(idP->value.s, regId) == 0)
      return DB_ALREADY_EXISTS;
  }

  CorNode* cloneP = corTreeClone(NULL, regP);
  if (cloneP == NULL)
  {
    KT_E("corDB: corTreeClone failed for registration '%s'", regId);
    return DB_ERR;
  }

  corTreeChildAdd(registrations, cloneP);

  return DB_OK;
}

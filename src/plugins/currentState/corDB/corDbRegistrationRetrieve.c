//
// FILE            corDbRegistrationRetrieve.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                   // strcmp

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corRest/CorRestState.h"                       // corRest
#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, Tenant
#include "currentState/corDB/corDbStore.h"          // corDbRegistrations
#include "currentState/corDB/corDbRegistrationRetrieve.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbRegistrationRetrieve -
//
int corDbRegistrationRetrieve(Tenant* tenantP, const char* regId, CorNode** regPP)
{
  COR_DB_READ(tenantP);

  CorNode* registrations = corDbRegistrations(tenantP);

  for (CorNode* rP = registrations->value.firstChildP; rP != NULL; rP = rP->next)
  {
    CorNode* idP = corTreeLookup(rP, "id");

    if (idP != NULL && idP->type == CorString && strcmp(idP->value.s, regId) == 0)
    {
      *regPP = corTreeClone(corRest.kallocP, rP);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

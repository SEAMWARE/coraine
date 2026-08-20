//
// FILE            corDbRegistrationRetrieve.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                   // strcmp

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjLookup.h"                           // kjLookup

#include "corRest/CorRestState.h"                       // corRest
#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, Tenant
#include "currentState/corDB/corDbStore.h"          // corDbRegistrations
#include "currentState/corDB/corDbRegistrationRetrieve.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbRegistrationRetrieve -
//
int corDbRegistrationRetrieve(Tenant* tenantP, const char* regId, KjNode** regPP)
{
  KjNode* registrations = corDbRegistrations(tenantP);

  for (KjNode* rP = registrations->value.firstChildP; rP != NULL; rP = rP->next)
  {
    KjNode* idP = kjLookup(rP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, regId) == 0)
    {
      *regPP = kjClone(corRest.kjsonP, rP);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

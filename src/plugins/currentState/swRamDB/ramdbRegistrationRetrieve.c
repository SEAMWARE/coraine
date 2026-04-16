//
// FILE            ramdbRegistrationRetrieve.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <string.h>                                   // strcmp

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjLookup.h"                           // kjLookup

#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, Tenant
#include "currentState/swRamDB/ramdbStore.h"          // ramdbRegistrations
#include "currentState/swRamDB/ramdbRegistrationRetrieve.h"  // Own interface



// -----------------------------------------------------------------------------
//
// ramdbRegistrationRetrieve -
//
int ramdbRegistrationRetrieve(Tenant* tenantP, const char* regId, KjNode** regPP)
{
  KjNode* registrations = ramdbRegistrations(tenantP);

  for (KjNode* rP = registrations->value.firstChildP; rP != NULL; rP = rP->next)
  {
    KjNode* idP = kjLookup(rP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, regId) == 0)
    {
      *regPP = kjClone(NULL, rP);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

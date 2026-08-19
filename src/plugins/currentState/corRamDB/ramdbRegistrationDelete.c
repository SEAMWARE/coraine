//
// FILE            ramdbRegistrationDelete.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <string.h>                                   // strcmp

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjChildRemove
#include "kjson/kjLookup.h"                           // kjLookup

#include "kjson/kjFree.h"                             // kjFree
#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, Tenant
#include "currentState/corRamDB/ramdbStore.h"          // ramdbRegistrations
#include "currentState/corRamDB/ramdbRegistrationDelete.h"  // Own interface



// -----------------------------------------------------------------------------
//
// ramdbRegistrationDelete -
//
int ramdbRegistrationDelete(Tenant* tenantP, const char* regId)
{
  KjNode* registrations = ramdbRegistrations(tenantP);

  for (KjNode* rP = registrations->value.firstChildP; rP != NULL; rP = rP->next)
  {
    KjNode* idP = kjLookup(rP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, regId) == 0)
    {
      kjChildRemove(registrations, rP);
      kjFree(rP);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

//
// FILE            ramdbRegistrationCreate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <string.h>                                   // strcmp

#include "ktrace/kTrace.h"                            // KT_E
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjBuilder.h"                          // kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup

#include "db/DbDriver.h"                              // DB_OK, DB_ALREADY_EXISTS, DB_ERR, Tenant
#include "currentState/corRamDB/ramdbStore.h"          // ramdbRegistrations
#include "currentState/corRamDB/ramdbRegistrationCreate.h"  // Own interface



// -----------------------------------------------------------------------------
//
// ramdbRegistrationCreate -
//
int ramdbRegistrationCreate(Tenant* tenantP, const char* regId, KjNode* regP)
{
  KjNode* registrations = ramdbRegistrations(tenantP);

  for (KjNode* rP = registrations->value.firstChildP; rP != NULL; rP = rP->next)
  {
    KjNode* idP = kjLookup(rP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, regId) == 0)
      return DB_ALREADY_EXISTS;
  }

  KjNode* cloneP = kjClone(NULL, regP);
  if (cloneP == NULL)
  {
    KT_E("corRamDB: kjClone failed for registration '%s'", regId);
    return DB_ERR;
  }

  kjChildAdd(registrations, cloneP);

  return DB_OK;
}

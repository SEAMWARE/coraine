//
// FILE            ramdbRegistrationUpdate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <string.h>                                   // strcmp

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjBuilder.h"                          // kjChildAdd, kjChildRemove
#include "kjson/kjLookup.h"                           // kjLookup

#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, Tenant
#include "currentState/swRamDB/ramdbStore.h"          // ramdbRegistrations
#include "currentState/swRamDB/ramdbRegistrationUpdate.h"  // Own interface



// -----------------------------------------------------------------------------
//
// ramdbRegistrationUpdate - JSON Merge Patch semantics
//
// For each field in the fragment:
//   - if null value:  remove the field from the stored registration
//   - otherwise:      replace (or add) the field in the stored registration
//
int ramdbRegistrationUpdate(Tenant* tenantP, const char* regId, KjNode* fragmentP)
{
  KjNode* registrations = ramdbRegistrations(tenantP);

  KjNode* regP = NULL;

  for (KjNode* rP = registrations->value.firstChildP; rP != NULL; rP = rP->next)
  {
    KjNode* idP = kjLookup(rP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, regId) == 0)
    {
      regP = rP;
      break;
    }
  }

  if (regP == NULL)
    return DB_NOT_FOUND;

  KjNode* next;

  for (KjNode* fieldP = fragmentP->value.firstChildP; fieldP != NULL; fieldP = next)
  {
    next = fieldP->next;

    KjNode* existingP = kjLookup(regP, fieldP->name);

    if (fieldP->type == KjNull)
    {
      if (existingP != NULL)
        kjChildRemove(regP, existingP);
    }
    else
    {
      if (existingP != NULL)
        kjChildRemove(regP, existingP);

      KjNode* cloneP = kjClone(NULL, fieldP);
      kjChildAdd(regP, cloneP);
    }
  }

  return DB_OK;
}

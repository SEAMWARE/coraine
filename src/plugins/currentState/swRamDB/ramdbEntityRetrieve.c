//
// FILE            ramdbEntityRetrieve.c
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
#include "currentState/swRamDB/ramdbStore.h"          // ramdbEntities
#include "currentState/swRamDB/ramdbEntityRetrieve.h" // Own interface



// -----------------------------------------------------------------------------
//
// ramdbEntityRetrieve -
//
int ramdbEntityRetrieve(Tenant* tenantP, const char* entityId, KjNode** entityPP)
{
  KjNode* entities = ramdbEntities(tenantP);

  for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
  {
    KjNode* idP = kjLookup(eP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, entityId) == 0)
    {
      *entityPP = kjClone(NULL, eP);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

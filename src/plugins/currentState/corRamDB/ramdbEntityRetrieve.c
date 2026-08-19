//
// FILE            ramdbEntityRetrieve.c
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
#include "currentState/corRamDB/ramdbStore.h"          // ramdbEntities
#include "currentState/corRamDB/ramdbEntityRetrieve.h" // Own interface



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
      // Clone into the request arena (freed at request end), matching mongoc's
      // retrieve. A NULL (malloc) clone would leak — no caller frees the result;
      // they all consume it within the request (render / merge / replace-copy).
      *entityPP = kjClone(corRest.kjsonP, eP);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

//
// FILE            ramdbEntityCreate.c
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
#include "currentState/swRamDB/ramdbStore.h"          // ramdbEntities
#include "currentState/swRamDB/ramdbEntityCreate.h"   // Own interface



// -----------------------------------------------------------------------------
//
// ramdbEntityCreate -
//
int ramdbEntityCreate(Tenant* tenantP, const char* entityId, KjNode* entityP)
{
  KjNode* entities = ramdbEntities(tenantP);

  //
  // Check for duplicate
  //
  for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
  {
    KjNode* idP = kjLookup(eP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, entityId) == 0)
      return DB_ALREADY_EXISTS;
  }

  //
  // Deep-clone the entity tree (using malloc, not a buffer allocator)
  //
  KjNode* cloneP = kjClone(NULL, entityP);
  if (cloneP == NULL)
  {
    KT_E("swRamDB: kjClone failed for entity '%s'", entityId);
    return DB_ERR;
  }

  kjChildAdd(entities, cloneP);

  return DB_OK;
}

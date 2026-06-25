//
// FILE            ramdbEntityDelete.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <string.h>                                   // strcmp

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjChildRemove
#include "kjson/kjFree.h"                             // kjFree
#include "kjson/kjLookup.h"                           // kjLookup

#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, Tenant
#include "currentState/swRamDB/ramdbStore.h"          // ramdbEntities
#include "currentState/swRamDB/ramdbEntityDelete.h"   // Own interface



// -----------------------------------------------------------------------------
//
// ramdbEntityDelete -
//
int ramdbEntityDelete(Tenant* tenantP, const char* entityId)
{
  KjNode* entities = ramdbEntities(tenantP);

  for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
  {
    KjNode* idP = kjLookup(eP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, entityId) == 0)
    {
      kjChildRemove(entities, eP);
      kjFree(eP);   // malloc store node — free it, no caller takes ownership
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

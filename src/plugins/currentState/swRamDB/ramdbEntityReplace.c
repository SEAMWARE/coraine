//
// FILE            ramdbEntityReplace.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <string.h>                                    // strcmp

#include "ktrace/kTrace.h"                             // KT_E
#include "kjson/KjNode.h"                              // KjNode
#include "kjson/kjClone.h"                             // kjClone
#include "kjson/kjLookup.h"                            // kjLookup
#include "kjson/kjBuilder.h"                           // kjChildRemove, kjChildAdd

#include "db/DbDriver.h"                               // DB_OK, DB_NOT_FOUND, DB_ERR, Tenant
#include "currentState/swRamDB/ramdbStore.h"           // ramdbEntities
#include "currentState/swRamDB/ramdbEntityReplace.h"   // Own interface



// -----------------------------------------------------------------------------
//
// ramdbEntityReplace -
//
int ramdbEntityReplace(Tenant* tenantP, const char* entityId, KjNode* newEntityP, KjNode** oldEntityPP)
{
  KjNode* entities = ramdbEntities(tenantP);

  for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
  {
    KjNode* idP = kjLookup(eP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, entityId) == 0)
    {
      KjNode* cloneP = kjClone(NULL, newEntityP);
      if (cloneP == NULL)
      {
        KT_E("swRamDB: kjClone failed for entity '%s'", entityId);
        return DB_ERR;
      }

      kjChildRemove(entities, eP);
      kjChildAdd(entities, cloneP);

      if (oldEntityPP != NULL)
        *oldEntityPP = eP;

      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

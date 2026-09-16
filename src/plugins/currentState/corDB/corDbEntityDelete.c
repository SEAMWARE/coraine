//
// FILE            corDbEntityDelete.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                 // bool
#include <string.h>                                   // strcmp

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjChildRemove
#include "kjson/kjFree.h"                             // kjFree
#include "kjson/kjLookup.h"                           // kjLookup

#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, Tenant
#include "currentState/corDB/corDbIndex.h"        // corDbIndexRemove
#include "currentState/corDB/corDbStore.h"          // corDbEntities
#include "currentState/corDB/corDbEntityDelete.h"   // Own interface



// -----------------------------------------------------------------------------
//
// corDbEntityDelete -
//
int corDbEntityDelete(Tenant* tenantP, const char* entityId)
{
  COR_DB_WRITE(tenantP);

  KjNode* entities = corDbEntities(tenantP);

  //
  // One hop via the id index instead of a walk of the whole store with a
  // kjLookup per entity. The loop shape is kept so the body below is unchanged:
  // indexed, it runs exactly once for the hit and not at all for a miss;
  // unindexed - a store that predates the index - it walks as it always did.
  //
  CorDbStore* idxStoreP = corDbStoreOf(tenantP);
  KjNode*     idxHitP   = corDbIndexLookup(idxStoreP, entityId);
  bool        indexed   = (idxStoreP != NULL) && (idxStoreP->idIndex != NULL);

  for (KjNode* eP = indexed ? idxHitP : entities->value.firstChildP;
       eP != NULL;
       eP = indexed ? NULL : eP->next)
  {
    KjNode* idP = kjLookup(eP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, entityId) == 0)
    {
      corDbIndexRemove(corDbStoreOf(tenantP), eP);
      kjChildRemove(entities, eP);
      kjFree(eP);   // malloc store node — free it, no caller takes ownership
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

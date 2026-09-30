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

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeFree.h"                      // corTreeFree
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, Tenant
#include "currentState/corDB/corDbIndex.h"        // corDbIndexLookup, corDbIndexUnlink
#include "currentState/corDB/corDbStore.h"          // corDbEntities
#include "currentState/corDB/corDbEntityDelete.h"   // Own interface



// -----------------------------------------------------------------------------
//
// corDbEntityDelete -
//
int corDbEntityDelete(Tenant* tenantP, const char* entityId)
{
  //
  // Unlinked under the write lock - O(1) through the index - and freed AFTER it: the free is a
  // whole entity's worth of free()s, and every other writer of the tenant waited them out.
  //
  CorNode* goneP = NULL;

  {
  COR_DB_WRITE(tenantP);

  CorNode* entities = corDbEntities(tenantP);

  //
  // One hop via the id index instead of a walk of the whole store with a
  // corTreeLookup per entity. The loop shape is kept so the body below is unchanged:
  // indexed, it runs exactly once for the hit and not at all for a miss;
  // unindexed - a store that predates the index - it walks as it always did.
  //
  CorDbStore* idxStoreP = corDbStoreOf(tenantP);
  CorNode*    idxHitP   = corDbIndexLookup(idxStoreP, entityId);
  bool        indexed   = (idxStoreP != NULL) && (idxStoreP->idToPrevEntity != NULL);

  for (CorNode* eP = indexed ? idxHitP : entities->value.head;
       eP != NULL;
       eP = indexed ? NULL : eP->next)
  {
    CorNode* idP = corTreeLookup(eP, "id");

    if (idP != NULL && idP->type == CorString && strcmp(idP->value.s, entityId) == 0)
    {
      corDbIndexUnlink(idxStoreP, eP);
      goneP = eP;
      break;
    }
  }
  }

  if (goneP == NULL)
    return DB_NOT_FOUND;

  corTreeFree(goneP);                                // malloc store node - no caller takes ownership
  return DB_OK;
}

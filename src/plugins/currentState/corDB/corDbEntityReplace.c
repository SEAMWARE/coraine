//
// FILE            corDbEntityReplace.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                 // bool
#include <string.h>                                    // strcmp

#include "ktrace/kTrace.h"                             // KT_E
#include "corTree/CorNode.h"                           // CorNode
#include "corTree/corTreeClone.h"                      // corTreeClone
#include "corTree/corTreeLookup.h"                     // corTreeLookup
#include "corTree/corTreeFree.h"                       // corTreeFree
#include "corTree/corTreeChildReplace.h"               // corTreeChildReplace
#include "corRest/CorRestState.h"                        // corRest

#include "db/DbDriver.h"                               // DB_OK, DB_NOT_FOUND, DB_ERR, Tenant
#include "currentState/corDB/corDbIndex.h"        // corDbIndexAdd, corDbIndexRemove
#include "currentState/corDB/corDbStore.h"           // corDbEntities
#include "currentState/corDB/corDbEntityReplace.h"   // Own interface



// -----------------------------------------------------------------------------
//
// corDbEntityReplace -
//
int corDbEntityReplace(Tenant* tenantP, const char* entityId, CorNode* newEntityP, CorNode** oldEntityPP)
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
  bool        indexed   = (idxStoreP != NULL) && (idxStoreP->idIndex != NULL);

  for (CorNode* eP = indexed ? idxHitP : entities->value.head;
       eP != NULL;
       eP = indexed ? NULL : eP->next)
  {
    CorNode* idP = corTreeLookup(eP, "id");

    if (idP != NULL && idP->type == CorString && strcmp(idP->value.s, entityId) == 0)
    {
      CorNode* cloneP = corTreeClone(NULL, newEntityP);
      if (cloneP == NULL)
      {
        KT_E("corDB: corTreeClone failed for entity '%s'", entityId);
        return DB_ERR;
      }

      // Replace in place so the entity keeps its store (creation-order)
      // position — a GET without orderBy stays stable and matches mongoc,
      // which preserves createdAt on Replace.
      //
      // The index points at the OLD node, which is about to be freed. Drop it
      // before the swap and add the new one after - an index entry surviving a
      // replace is a pointer to freed memory that every later lookup returns.
      //
      corDbIndexRemove(corDbStoreOf(tenantP), eP);
      corTreeChildReplace(entities, eP, cloneP);
      corDbIndexAdd(corDbStoreOf(tenantP), cloneP);

      // Hand the caller a request-arena copy of the pre-replace entity (freed at
      // request end, matching mongoc's oldEntityPP), then free the malloc store
      // node — returning the raw malloc node would leak (no caller frees it).
      if (oldEntityPP != NULL)
        *oldEntityPP = corTreeClone(corRest.kallocP, eP);
      corTreeFree(eP);

      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

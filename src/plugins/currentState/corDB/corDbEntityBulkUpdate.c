//
// FILE            corDbEntityBulkUpdate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corDB has no native bulk primitive — we walk the input array and
// replace each existing entity with its caller-supplied, already-merged
// final state. Per-entity outcome is written to resultsV so the service
// routine can assemble the BatchOperationResult.
//

#include <stdlib.h>                                      // malloc, free
#include <string.h>                                      // strcmp

#include "corLog/corLog.h"                               // COR_E
#include "corTree/CorNode.h"                             // CorNode
#include "corTree/corTreeClone.h"                        // corTreeClone
#include "corTree/corTreeFree.h"                         // corTreeFree
#include "corTree/corTreeChildReplace.h"                 // corTreeChildReplace
#include "corTree/corTreeLookup.h"                       // corTreeLookup

#include "db/DbDriver.h"                                 // DB_OK, DB_NOT_FOUND, DB_ERR, Tenant
#include "currentState/corDB/corDbIndex.h"        // corDbIndexLookup, corDbIndexReplace
#include "currentState/corDB/corDbStore.h"             // corDbEntities
#include "currentState/corDB/corDbEntityBulkUpdate.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbEntityBulkUpdate -
//
int corDbEntityBulkUpdate(Tenant* tenantP, CorNode* entitiesArr, int* resultsV)
{
  if (entitiesArr == NULL || entitiesArr->type != CorArray)
    return DB_ERR;

  //
  // Clones BEFORE the write lock, the replaced entities freed AFTER it - only the swaps need
  // the lock (see corDbEntityBulkCreate for what doing the rest under it cost). An entity
  // swapped out and dropped from the index is reachable by nobody else, so it is parked in
  // the same slot of cloneV (its clone went into the store) until the lock is released.
  //
  int count = 0;

  for (CorNode* inP = entitiesArr->value.head; inP != NULL; inP = inP->next)
    ++count;

  CorNode** cloneV = (count > 0) ? (CorNode**) malloc(count * sizeof(CorNode*)) : NULL;

  if ((count > 0) && (cloneV == NULL))
    return DB_ERR;

  int ix = 0;

  for (CorNode* inP = entitiesArr->value.head; inP != NULL; inP = inP->next, ix++)
  {
    CorNode* idP = corTreeLookup(inP, "id");

    cloneV[ix] = ((idP != NULL) && (idP->type == CorString)) ? corTreeClone(NULL, inP) : NULL;
  }

  bool anyOk = false;

  {
    COR_DB_WRITE(tenantP);

    CorNode*    entities  = corDbEntities(tenantP);
    CorDbStore* idxStoreP = corDbStoreOf(tenantP);
    bool        indexed   = (idxStoreP != NULL) && (idxStoreP->idToPrevEntity != NULL);

    ix = 0;
    for (CorNode* inP = entitiesArr->value.head; inP != NULL; inP = inP->next, ix++)
    {
      CorNode* idP = corTreeLookup(inP, "id");
      if (idP == NULL || idP->type != CorString)
      {
        resultsV[ix] = DB_ERR;
        continue;
      }

      //
      // Locate the existing entity by id - one hop via the id index. This walked the WHOLE
      // store for every entity of the batch, under the write lock, the same walk that cost
      // corDbEntityBulkCreate a factor of 24. The walk stays as the fallback for a store with
      // no index: a NULL index must not mean "not there".
      //
      CorNode* existing = indexed ? corDbIndexLookup(idxStoreP, idP->value.s) : NULL;

      if (indexed == false)
      {
        for (CorNode* eP = entities->value.head; eP != NULL; eP = eP->next)
        {
          CorNode* existingId = corTreeLookup(eP, "id");
          if (existingId != NULL && existingId->type == CorString &&
              strcmp(existingId->value.s, idP->value.s) == 0)
          {
            existing = eP;
            break;
          }
        }
      }

      if (existing == NULL)
      {
        resultsV[ix] = DB_NOT_FOUND;
        continue;
      }

      CorNode* cloneP = cloneV[ix];
      if (cloneP == NULL)
      {
        COR_E("corDB: corTreeClone failed for entity '%s'", idP->value.s);
        resultsV[ix] = DB_ERR;
        continue;
      }

      // Replace in place so the entity keeps its store (creation-order) position
      // — a GET without orderBy stays stable and matches mongoc, which preserves
      // createdAt on update. corTreeChildReplace does not free the old node.
      //
      // Through the index, which swaps in O(1) and re-points the successor's entry, which
      // named `existing` as its predecessor - see corDbIndex.c.
      //
      corDbIndexReplace(idxStoreP, existing, cloneP);
      cloneV[ix]   = existing;   // stored clone out, replaced entity in - freed below, unlocked
      resultsV[ix] = DB_OK;
      anyOk        = true;
    }
  }

  //
  // Every slot now holds something the store does not: an entity that was replaced, or a clone
  // that was not used (not found, no id)
  //
  for (ix = 0; ix < count; ix++)
  {
    if (cloneV[ix] != NULL)
      corTreeFree(cloneV[ix]);
  }

  free(cloneV);

  return anyOk ? DB_OK : DB_ERR;
}

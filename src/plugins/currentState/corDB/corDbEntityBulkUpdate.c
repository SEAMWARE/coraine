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

#include <string.h>                                      // strcmp

#include "corLog/corLog.h"                               // COR_E
#include "corTree/CorNode.h"                             // CorNode
#include "corTree/corTreeClone.h"                        // corTreeClone
#include "corTree/corTreeFree.h"                         // corTreeFree
#include "corTree/corTreeChildReplace.h"                 // corTreeChildReplace
#include "corTree/corTreeLookup.h"                       // corTreeLookup

#include "db/DbDriver.h"                                 // DB_OK, DB_NOT_FOUND, DB_ERR, Tenant
#include "currentState/corDB/corDbIndex.h"        // corDbIndexAdd, corDbIndexRemove
#include "currentState/corDB/corDbStore.h"             // corDbEntities
#include "currentState/corDB/corDbEntityBulkUpdate.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbEntityBulkUpdate -
//
int corDbEntityBulkUpdate(Tenant* tenantP, CorNode* entitiesArr, int* resultsV)
{
  COR_DB_WRITE(tenantP);

  if (entitiesArr == NULL || entitiesArr->type != CorArray)
    return DB_ERR;

  CorNode* entities = corDbEntities(tenantP);
  int     ix       = 0;
  bool    anyOk    = false;

  for (CorNode* inP = entitiesArr->value.head; inP != NULL; inP = inP->next, ix++)
  {
    CorNode* idP = corTreeLookup(inP, "id");
    if (idP == NULL || idP->type != CorString)
    {
      resultsV[ix] = DB_ERR;
      continue;
    }

    // Locate the existing entity by id
    CorNode* existing = NULL;
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

    if (existing == NULL)
    {
      resultsV[ix] = DB_NOT_FOUND;
      continue;
    }

    CorNode* cloneP = corTreeClone(NULL, inP);
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
    // The index points at `existing`, which corTreeFree is about to destroy. Drop it
    // before the swap and add the replacement after, or every later lookup of
    // this id returns a pointer into freed memory.
    //
    corDbIndexRemove(corDbStoreOf(tenantP), existing);
    corTreeChildReplace(entities, existing, cloneP);
    corDbIndexAdd(corDbStoreOf(tenantP), cloneP);
    corTreeFree(existing);
    resultsV[ix] = DB_OK;
    anyOk        = true;
  }

  return anyOk ? DB_OK : DB_ERR;
}

//
// FILE            corDbEntityBulkCreate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corDB is a malloc-backed in-process store — there is no bulk-write
// primitive to exploit. We walk the input array, honour the first-wins
// invariant against whatever is already in the store, and report a
// per-entity result so the service routine can assemble the
// BatchOperationResult.
//

#include <stdlib.h>                                    // malloc, free
#include <string.h>                                    // strcmp

#include "corLog/corLog.h"                             // COR_E
#include "corTree/CorNode.h"                           // CorNode
#include "corTree/corTreeClone.h"                      // corTreeClone
#include "corTree/corTreeFree.h"                       // corTreeFree
#include "corTree/corTreeBuilder.h"                    // corTreeChildAdd
#include "corTree/corTreeLookup.h"                     // corTreeLookup

#include "db/DbDriver.h"                               // DB_OK, DB_ALREADY_EXISTS, DB_ERR, Tenant
#include "currentState/corDB/corDbIndex.h"        // corDbIndexAdd, corDbIndexLookup
#include "currentState/corDB/corDbStore.h"           // corDbEntities
#include "currentState/corDB/corDbEntityBulkCreate.h"// Own interface



// -----------------------------------------------------------------------------
//
// corDbEntityBulkCreate -
//
int corDbEntityBulkCreate(Tenant* tenantP, CorNode* entitiesArr, int* resultsV)
{
  if (entitiesArr == NULL || entitiesArr->type != CorArray)
    return DB_ERR;

  //
  // Clone BEFORE taking the lock.
  //
  // The clone is the deep copy into the store's own heap nodes - hundreds of mallocs for a
  // batch - and it reads nothing but the request's tree, which no other thread can see. Done
  // under the write lock it was most of the time the lock was held, and every other writer of
  // the tenant waited it out: batch create on one tenant was capped by it, and got SLOWER as
  // the request path got faster (more threads arriving at the lock sooner, each contended
  // acquisition a futex sleep and wake). Under the lock now: the existence check, the link,
  // the index insert. A clone that turns out not to be needed (the id exists) is freed.
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

  COR_DB_WRITE(tenantP);

  CorNode* entities = corDbEntities(tenantP);
  bool     anyOk    = false;

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
    // First-wins within this batch + against the store.
    //
    // O(1) via the id index, and it covers both halves of that sentence without
    // needing to: an entity created earlier in this same batch was added to the
    // index as it was added to the store, so it is found here like any other.
    //
    // This walked the WHOLE entity list for every incoming entity, with a
    // corTreeLookup per comparison, while the index sat right there being maintained
    // by the bottom of this very loop. It cost a factor of 24 against the batch
    // UPDATE endpoint next door - 530 requests/s where batchUpdate does 12 968 -
    // and it got worse as the store grew, because a batch create is the one
    // operation that makes the store it is scanning bigger with every entity.
    //
    // The walk stays as the fallback for a store with no index, on the same
    // reasoning as corDbEntityRetrieve: a NULL index must not mean "not there".
    //
    bool exists = false;

    if (corDbIndexLookup(corDbStoreOf(tenantP), idP->value.s) != NULL)
      exists = true;
    else if (corDbStoreOf(tenantP)->idIndex == NULL)
    {
      for (CorNode* eP = entities->value.head; eP != NULL; eP = eP->next)
      {
        CorNode* existingId = corTreeLookup(eP, "id");
        if (existingId != NULL && existingId->type == CorString &&
            strcmp(existingId->value.s, idP->value.s) == 0)
        {
          exists = true;
          break;
        }
      }
    }

    CorNode* cloneP = cloneV[ix];

    if (exists)
    {
      if (cloneP != NULL)
        corTreeFree(cloneP);

      resultsV[ix] = DB_ALREADY_EXISTS;
      continue;
    }

    if (cloneP == NULL)
    {
      COR_E("corDB: corTreeClone failed for entity '%s'", idP->value.s);
      resultsV[ix] = DB_ERR;
      continue;
    }

    corTreeChildAdd(entities, cloneP);
    corDbIndexAdd(corDbStoreOf(tenantP), cloneP);
    resultsV[ix] = DB_OK;
    anyOk        = true;
  }

  free(cloneV);

  return anyOk ? DB_OK : DB_ERR;
}

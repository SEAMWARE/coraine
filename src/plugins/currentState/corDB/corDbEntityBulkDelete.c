//
// FILE            corDbEntityBulkDelete.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corDB Batch Delete: per id, clone the stored entity into the
// request arena (for the service's delete notification), then remove
// the entity from the tenant store.
//

#include <stdbool.h>                                      // bool
#include <stddef.h>                                       // NULL
#include <stdlib.h>                                       // calloc, free
#include <string.h>                                       // strcmp

#include "corTree/CorNode.h"                              // CorNode
#include "corTree/corTreeClone.h"                         // corTreeClone
#include "corTree/corTreeFree.h"                          // corTreeFree

#include "corRest/CorRestState.h"                           // corRest (kallocP arena)

#include "db/DbDriver.h"                                  // DB_OK, DB_NOT_FOUND, DB_ERR, Tenant
#include "currentState/corDB/corDbIndex.h"        // corDbIndexLookup, corDbIndexUnlink, corDbEntityId
#include "currentState/corDB/corDbStore.h"              // corDbEntities
#include "currentState/corDB/corDbEntityBulkDelete.h"   // Own interface



// -----------------------------------------------------------------------------
//
// corDbEntityBulkDelete -
//
int corDbEntityBulkDelete(Tenant* tenantP, const char** idV, int N,
                          int* resultsV, CorNode** snapshotsV)
{
  //
  // Under the write lock only what must be: find each entity - through the id index, one hash, where
  // this used to walk the whole store per id - and unlink it, O(1) through the same index. The
  // snapshot for the notifications and the free come AFTER the lock: they are the bulk of the work,
  // and every other writer of the tenant waited them out (2026-09-30, corDB, 40 000 entities: 709
  // batches of 20 a second, p99 678 ms).
  //
  CorNode** goneV = (CorNode**) calloc((N > 0) ? N : 1, sizeof(CorNode*));
  bool      anyOk = false;

  if (goneV == NULL)
    return DB_ERR;

  {
    COR_DB_WRITE(tenantP);

    CorDbStore* storeP   = corDbStoreOf(tenantP);
    CorNode*    entities = corDbEntities(tenantP);
    bool        indexed  = (storeP != NULL) && (storeP->idToPrevEntity != NULL);

    for (int i = 0; i < N; i++)
    {
      CorNode* match = NULL;

      if (indexed)
        match = corDbIndexLookup(storeP, idV[i]);
      else
      {
        for (CorNode* eP = entities->value.head; eP != NULL; eP = eP->next)
        {
          const char* storedId = corDbEntityId(eP);

          if ((storedId != NULL) && (strcmp(storedId, idV[i]) == 0))
          {
            match = eP;
            break;
          }
        }
      }

      if (match == NULL)
      {
        resultsV[i] = DB_NOT_FOUND;                 // not there - or an id this batch named twice
        continue;
      }

      corDbIndexUnlink(storeP, match);
      goneV[i]    = match;
      resultsV[i] = DB_OK;
      anyOk       = true;
    }
  }

  for (int i = 0; i < N; i++)
  {
    snapshotsV[i] = NULL;

    if (goneV[i] != NULL)
    {
      snapshotsV[i] = corTreeClone(corRest.kallocP, goneV[i]);   // arena snapshot for notify
      corTreeFree(goneV[i]);                                      // the malloc store node
    }
  }

  free(goneV);

  return anyOk ? DB_OK : DB_ERR;
}

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

#include <stddef.h>                                       // NULL
#include <string.h>                                       // strcmp

#include "corTree/CorNode.h"                              // CorNode
#include "corTree/corTreeBuilder.h"                       // corTreeChildRemove
#include "corTree/corTreeClone.h"                         // corTreeClone
#include "corTree/corTreeFree.h"                          // corTreeFree
#include "corTree/corTreeLookup.h"                        // corTreeLookup

#include "corRest/CorRestState.h"                           // corRest (kallocP arena)

#include "db/DbDriver.h"                                  // DB_OK, DB_NOT_FOUND, DB_ERR, Tenant
#include "currentState/corDB/corDbIndex.h"        // corDbIndexRemove
#include "currentState/corDB/corDbStore.h"              // corDbEntities
#include "currentState/corDB/corDbEntityBulkDelete.h"   // Own interface



// -----------------------------------------------------------------------------
//
// corDbEntityBulkDelete -
//
int corDbEntityBulkDelete(Tenant* tenantP, const char** idV, int N,
                          int* resultsV, CorNode** snapshotsV)
{
  COR_DB_WRITE(tenantP);

  CorNode* entities = corDbEntities(tenantP);
  bool    anyOk    = false;

  for (int i = 0; i < N; i++)
  {
    CorNode* match = NULL;
    for (CorNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
    {
      CorNode* storedIdP = corTreeLookup(eP, "id");
      if (storedIdP != NULL && storedIdP->type == CorString &&
          strcmp(storedIdP->value.s, idV[i]) == 0)
      {
        match = eP;
        break;
      }
    }

    if (match == NULL)
    {
      resultsV[i]   = DB_NOT_FOUND;
      snapshotsV[i] = NULL;
      continue;
    }

    snapshotsV[i] = corTreeClone(corRest.kallocP, match); // arena snapshot for notify
    corDbIndexRemove(corDbStoreOf(tenantP), match);
    corTreeChildRemove(entities, match);
    corTreeFree(match);                               // free the malloc store node
    resultsV[i] = DB_OK;
    anyOk       = true;
  }

  return anyOk ? DB_OK : DB_ERR;
}

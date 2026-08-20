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

#include <string.h>                                    // strcmp

#include "ktrace/kTrace.h"                             // KT_E
#include "kjson/KjNode.h"                              // KjNode
#include "kjson/kjClone.h"                             // kjClone
#include "kjson/kjBuilder.h"                           // kjChildAdd
#include "kjson/kjLookup.h"                            // kjLookup

#include "db/DbDriver.h"                               // DB_OK, DB_ALREADY_EXISTS, DB_ERR, Tenant
#include "currentState/corDB/corDbStore.h"           // corDbEntities
#include "currentState/corDB/corDbEntityBulkCreate.h"// Own interface



// -----------------------------------------------------------------------------
//
// corDbEntityBulkCreate -
//
int corDbEntityBulkCreate(Tenant* tenantP, KjNode* entitiesArr, int* resultsV)
{
  if (entitiesArr == NULL || entitiesArr->type != KjArray)
    return DB_ERR;

  KjNode* entities = corDbEntities(tenantP);
  int     ix       = 0;
  bool    anyOk    = false;

  for (KjNode* inP = entitiesArr->value.firstChildP; inP != NULL; inP = inP->next, ix++)
  {
    KjNode* idP = kjLookup(inP, "id");
    if (idP == NULL || idP->type != KjString)
    {
      resultsV[ix] = DB_ERR;
      continue;
    }

    // First-wins within this batch + against the store
    bool exists = false;
    for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
    {
      KjNode* existingId = kjLookup(eP, "id");
      if (existingId != NULL && existingId->type == KjString &&
          strcmp(existingId->value.s, idP->value.s) == 0)
      {
        exists = true;
        break;
      }
    }

    if (exists)
    {
      resultsV[ix] = DB_ALREADY_EXISTS;
      continue;
    }

    KjNode* cloneP = kjClone(NULL, inP);
    if (cloneP == NULL)
    {
      KT_E("corDB: kjClone failed for entity '%s'", idP->value.s);
      resultsV[ix] = DB_ERR;
      continue;
    }

    kjChildAdd(entities, cloneP);
    resultsV[ix] = DB_OK;
    anyOk        = true;
  }

  return anyOk ? DB_OK : DB_ERR;
}

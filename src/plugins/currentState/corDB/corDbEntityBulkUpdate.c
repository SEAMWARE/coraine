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

#include "ktrace/kTrace.h"                               // KT_E
#include "kjson/KjNode.h"                                // KjNode
#include "kjson/kjClone.h"                               // kjClone
#include "kjson/kjFree.h"                                // kjFree
#include "kjson/kjChildReplace.h"                        // kjChildReplace
#include "kjson/kjLookup.h"                              // kjLookup

#include "db/DbDriver.h"                                 // DB_OK, DB_NOT_FOUND, DB_ERR, Tenant
#include "currentState/corDB/corDbStore.h"             // corDbEntities
#include "currentState/corDB/corDbEntityBulkUpdate.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbEntityBulkUpdate -
//
int corDbEntityBulkUpdate(Tenant* tenantP, KjNode* entitiesArr, int* resultsV)
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

    // Locate the existing entity by id
    KjNode* existing = NULL;
    for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
    {
      KjNode* existingId = kjLookup(eP, "id");
      if (existingId != NULL && existingId->type == KjString &&
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

    KjNode* cloneP = kjClone(NULL, inP);
    if (cloneP == NULL)
    {
      KT_E("corDB: kjClone failed for entity '%s'", idP->value.s);
      resultsV[ix] = DB_ERR;
      continue;
    }

    // Replace in place so the entity keeps its store (creation-order) position
    // — a GET without orderBy stays stable and matches mongoc, which preserves
    // createdAt on update. kjChildReplace does not free the old node.
    kjChildReplace(entities, existing, cloneP);
    kjFree(existing);
    resultsV[ix] = DB_OK;
    anyOk        = true;
  }

  return anyOk ? DB_OK : DB_ERR;
}

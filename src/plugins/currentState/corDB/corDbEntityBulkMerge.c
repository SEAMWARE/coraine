//
// FILE            corDbEntityBulkMerge.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corDB Batch Merge persistence — the store is in-memory, so the two phases
// the broker brackets the merge with are plain loops:
//
//   corDbEntityBulkRetrieve     clone each current stored entity into the
//                               request arena (same-id fragments share one
//                               clone so the broker's sequential merges
//                               accumulate). The broker then runs ldEntityMerge.
//   corDbEntityBulkChangesApply apply each fragment's change report to the live
//                               stored entity.
//

#include <stddef.h>                                       // NULL
#include <string.h>                                       // strcmp

#include "corTree/CorNode.h"                              // CorNode
#include "corTree/corTreeClone.h"                         // corTreeClone
#include "corTree/corTreeLookup.h"                        // corTreeLookup

#include "corRest/CorRestState.h"                           // corRest (kallocP for arena clones)

#include "corNgsild/ldEntityMerge.h"                       // LdMergeReport

#include "db/DbDriver.h"                                  // DB_OK, DB_ERR, Tenant
#include "currentState/corDB/corDbStore.h"              // corDbEntities
#include "currentState/corDB/corDbEntityMerge.h"        // corDbApplyReportToLive
#include "currentState/corDB/corDbEntityBulkMerge.h"    // Own interface



// -----------------------------------------------------------------------------
//
// fragmentAt - the ix-th child of a CorArray
//
static CorNode* fragmentAt(CorNode* arrP, int ix)
{
  int i = 0;
  for (CorNode* c = arrP->value.head; c != NULL; c = c->next, i++)
    if (i == ix) return c;
  return NULL;
}



// -----------------------------------------------------------------------------
//
// liveById - locate the live stored entity with the given id
//
static CorNode* liveById(CorNode* entities, const char* id)
{
  for (CorNode* eP = entities->value.head; eP != NULL; eP = eP->next)
  {
    CorNode* idP = corTreeLookup(eP, "id");
    if (idP != NULL && idP->type == CorString && strcmp(idP->value.s, id) == 0)
      return eP;
  }
  return NULL;
}



// -----------------------------------------------------------------------------
//
// corDbEntityBulkRetrieve - Batch Merge Phase 1: clone current stored entities.
//
// `targetsV` is a caller-allocated, zeroed array parallel to the fragments.
// Each slot gets a request-arena clone of the live entity, or stays NULL when
// no such entity exists. Same-id fragments share one clone.
//
int corDbEntityBulkRetrieve(Tenant* tenantP, CorNode* fragmentsArr, CorNode** targetsV)
{
  COR_DB_READ(tenantP);

  if (fragmentsArr == NULL || fragmentsArr->type != CorArray)
    return DB_ERR;

  CorNode* entities = corDbEntities(tenantP);

  int k = 0;
  for (CorNode* fragP = fragmentsArr->value.head; fragP != NULL; fragP = fragP->next, k++)
  {
    if (targetsV[k] != NULL)
      continue;

    CorNode* idP = corTreeLookup(fragP, "id");
    if (idP == NULL || idP->type != CorString)
      continue;

    CorNode* live = liveById(entities, idP->value.s);
    if (live == NULL)
      continue;  // slot stays NULL -> DB_NOT_FOUND in the broker

    CorNode* shared = corTreeClone(corRest.kallocP, live);

    int j = 0;
    for (CorNode* f2 = fragmentsArr->value.head; f2 != NULL; f2 = f2->next, j++)
    {
      if (targetsV[j] != NULL)
        continue;
      CorNode* id2 = corTreeLookup(f2, "id");
      if (id2 != NULL && id2->type == CorString && strcmp(id2->value.s, idP->value.s) == 0)
        targetsV[j] = shared;
    }
  }

  return DB_OK;
}



// -----------------------------------------------------------------------------
//
// corDbEntityBulkChangesApply - Batch Merge Phase 2: apply each fragment's
// change report to its live stored entity.
//
int corDbEntityBulkChangesApply(Tenant* tenantP, CorNode* fragmentsArr,
                                CorNode** mergedTargetsV, LdMergeReport* reportsV,
                                int* resultsV)
{
  COR_DB_WRITE(tenantP);

  if (fragmentsArr == NULL || fragmentsArr->type != CorArray)
    return DB_ERR;

  CorNode* entities = corDbEntities(tenantP);

  int  n     = 0;
  for (CorNode* c = fragmentsArr->value.head; c != NULL; c = c->next) n++;

  bool anyOk = false;

  for (int i = 0; i < n; i++)
  {
    if (resultsV[i] != DB_OK || mergedTargetsV[i] == NULL)
      continue;

    CorNode* fragP = fragmentAt(fragmentsArr, i);
    CorNode* idP  = (fragP != NULL) ? corTreeLookup(fragP, "id") : NULL;
    if (idP == NULL || idP->type != CorString)
      continue;

    CorNode* live = liveById(entities, idP->value.s);
    if (live != NULL)
    {
      corDbApplyReportToLive(live, mergedTargetsV[i], &reportsV[i]);
      anyOk = true;
    }
  }

  return anyOk ? DB_OK : DB_ERR;
}

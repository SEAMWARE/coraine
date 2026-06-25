//
// FILE            ramdbEntityBulkMerge.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// swRamDB Batch Merge persistence — the store is in-memory, so the two phases
// the broker brackets the merge with are plain loops:
//
//   ramdbEntityBulkRetrieve     clone each current stored entity into the
//                               request arena (same-id fragments share one
//                               clone so the broker's sequential merges
//                               accumulate). The broker then runs ldEntityMerge.
//   ramdbEntityBulkChangesApply apply each fragment's change report to the live
//                               stored entity.
//

#include <stddef.h>                                       // NULL
#include <string.h>                                       // strcmp

#include "kjson/KjNode.h"                                 // KjNode
#include "kjson/kjClone.h"                                // kjClone
#include "kjson/kjLookup.h"                               // kjLookup

#include "swRest/SwRestState.h"                           // swRest (kjsonP for arena clones)

#include "swNgsild/ldEntityMerge.h"                       // LdMergeReport

#include "db/DbDriver.h"                                  // DB_OK, DB_ERR, Tenant
#include "currentState/swRamDB/ramdbStore.h"              // ramdbEntities
#include "currentState/swRamDB/ramdbEntityMerge.h"        // ramdbApplyReportToLive
#include "currentState/swRamDB/ramdbEntityBulkMerge.h"    // Own interface



// -----------------------------------------------------------------------------
//
// fragmentAt - the ix-th child of a KjArray
//
static KjNode* fragmentAt(KjNode* arrP, int ix)
{
  int i = 0;
  for (KjNode* c = arrP->value.firstChildP; c != NULL; c = c->next, i++)
    if (i == ix) return c;
  return NULL;
}



// -----------------------------------------------------------------------------
//
// liveById - locate the live stored entity with the given id
//
static KjNode* liveById(KjNode* entities, const char* id)
{
  for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
  {
    KjNode* idP = kjLookup(eP, "id");
    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, id) == 0)
      return eP;
  }
  return NULL;
}



// -----------------------------------------------------------------------------
//
// ramdbEntityBulkRetrieve - Batch Merge Phase 1: clone current stored entities.
//
// `targetsV` is a caller-allocated, zeroed array parallel to the fragments.
// Each slot gets a request-arena clone of the live entity, or stays NULL when
// no such entity exists. Same-id fragments share one clone.
//
int ramdbEntityBulkRetrieve(Tenant* tenantP, KjNode* fragmentsArr, KjNode** targetsV)
{
  if (fragmentsArr == NULL || fragmentsArr->type != KjArray)
    return DB_ERR;

  KjNode* entities = ramdbEntities(tenantP);

  int k = 0;
  for (KjNode* fragP = fragmentsArr->value.firstChildP; fragP != NULL; fragP = fragP->next, k++)
  {
    if (targetsV[k] != NULL)
      continue;

    KjNode* idP = kjLookup(fragP, "id");
    if (idP == NULL || idP->type != KjString)
      continue;

    KjNode* live = liveById(entities, idP->value.s);
    if (live == NULL)
      continue;  // slot stays NULL -> DB_NOT_FOUND in the broker

    KjNode* shared = kjClone(swRest.kjsonP, live);

    int j = 0;
    for (KjNode* f2 = fragmentsArr->value.firstChildP; f2 != NULL; f2 = f2->next, j++)
    {
      if (targetsV[j] != NULL)
        continue;
      KjNode* id2 = kjLookup(f2, "id");
      if (id2 != NULL && id2->type == KjString && strcmp(id2->value.s, idP->value.s) == 0)
        targetsV[j] = shared;
    }
  }

  return DB_OK;
}



// -----------------------------------------------------------------------------
//
// ramdbEntityBulkChangesApply - Batch Merge Phase 2: apply each fragment's
// change report to its live stored entity.
//
int ramdbEntityBulkChangesApply(Tenant* tenantP, KjNode* fragmentsArr,
                                KjNode** mergedTargetsV, LdMergeReport* reportsV,
                                int* resultsV)
{
  if (fragmentsArr == NULL || fragmentsArr->type != KjArray)
    return DB_ERR;

  KjNode* entities = ramdbEntities(tenantP);

  int  n     = 0;
  for (KjNode* c = fragmentsArr->value.firstChildP; c != NULL; c = c->next) n++;

  bool anyOk = false;

  for (int i = 0; i < n; i++)
  {
    if (resultsV[i] != DB_OK || mergedTargetsV[i] == NULL)
      continue;

    KjNode* fragP = fragmentAt(fragmentsArr, i);
    KjNode* idP   = (fragP != NULL) ? kjLookup(fragP, "id") : NULL;
    if (idP == NULL || idP->type != KjString)
      continue;

    KjNode* live = liveById(entities, idP->value.s);
    if (live != NULL)
    {
      ramdbApplyReportToLive(live, mergedTargetsV[i], &reportsV[i]);
      anyOk = true;
    }
  }

  return anyOk ? DB_OK : DB_ERR;
}

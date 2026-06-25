//
// FILE            ramdbEntityBulkMerge.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// swRamDB Batch Merge: the store is in-memory, so bulk is a plain loop
// over ramdbEntityMergeOne. Each fragment carries its id; results and
// merge-reports land in caller-allocated parallel arrays.
//

#include <stddef.h>                                       // NULL
#include <string.h>                                       // strcmp

#include "kjson/KjNode.h"                                 // KjNode
#include "kjson/kjClone.h"                                // kjClone
#include "kjson/kjLookup.h"                               // kjLookup

#include "swRest/SwRestState.h"                           // swRest (kjsonP for snapshot arena)

#include "swNgsild/ldEntityMerge.h"                       // LdMergeReport

#include "db/DbDriver.h"                                  // DB_OK, DB_ERR, Tenant
#include "currentState/swRamDB/ramdbStore.h"              // ramdbEntities
#include "currentState/swRamDB/ramdbEntityMerge.h"        // ramdbEntityMergeOne
#include "currentState/swRamDB/ramdbEntityBulkMerge.h"    // Own interface



// -----------------------------------------------------------------------------
//
// ramdbEntityBulkMerge -
//
int ramdbEntityBulkMerge(Tenant* tenantP, KjNode* fragmentsArr,
                         uint64_t ts, int* resultsV,
                         LdMergeReport* reportsV,
                         KjNode** snapshotsV)
{
  if (fragmentsArr == NULL || fragmentsArr->type != KjArray)
    return DB_ERR;

  KjNode* entities = ramdbEntities(tenantP);
  int     ix       = 0;
  bool    anyOk    = false;

  for (KjNode* fragP = fragmentsArr->value.firstChildP; fragP != NULL; fragP = fragP->next, ix++)
  {
    KjNode* idP = kjLookup(fragP, "id");
    if (idP == NULL || idP->type != KjString)
    {
      resultsV[ix] = DB_ERR;
      continue;
    }

    int r = ramdbEntityMergeOne(entities, idP->value.s, fragP, ts, &reportsV[ix], true);
    resultsV[ix] = r;

    if (r == DB_OK)
    {
      anyOk = true;

      // Post-merge snapshot — clone the live stored entity into the
      // request arena so the service can defer-notify without touching
      // tenant-store memory.
      for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
      {
        KjNode* storedIdP = kjLookup(eP, "id");
        if (storedIdP != NULL && storedIdP->type == KjString &&
            strcmp(storedIdP->value.s, idP->value.s) == 0)
        {
          snapshotsV[ix] = kjClone(swRest.kjsonP, eP);
          break;
        }
      }
    }
  }

  return anyOk ? DB_OK : DB_ERR;
}

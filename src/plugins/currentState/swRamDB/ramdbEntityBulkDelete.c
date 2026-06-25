//
// FILE            ramdbEntityBulkDelete.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// swRamDB Batch Delete: per id, clone the stored entity into the
// request arena (for the service's delete notification), then remove
// the entity from the tenant store.
//

#include <stddef.h>                                       // NULL
#include <string.h>                                       // strcmp

#include "kjson/KjNode.h"                                 // KjNode
#include "kjson/kjBuilder.h"                              // kjChildRemove
#include "kjson/kjClone.h"                                // kjClone
#include "kjson/kjFree.h"                                 // kjFree
#include "kjson/kjLookup.h"                               // kjLookup

#include "swRest/SwRestState.h"                           // swRest (kjsonP arena)

#include "db/DbDriver.h"                                  // DB_OK, DB_NOT_FOUND, DB_ERR, Tenant
#include "currentState/swRamDB/ramdbStore.h"              // ramdbEntities
#include "currentState/swRamDB/ramdbEntityBulkDelete.h"   // Own interface



// -----------------------------------------------------------------------------
//
// ramdbEntityBulkDelete -
//
int ramdbEntityBulkDelete(Tenant* tenantP, const char** idV, int N,
                          int* resultsV, KjNode** snapshotsV)
{
  KjNode* entities = ramdbEntities(tenantP);
  bool    anyOk    = false;

  for (int i = 0; i < N; i++)
  {
    KjNode* match = NULL;
    for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
    {
      KjNode* storedIdP = kjLookup(eP, "id");
      if (storedIdP != NULL && storedIdP->type == KjString &&
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

    snapshotsV[i] = kjClone(swRest.kjsonP, match);   // arena snapshot for notify
    kjChildRemove(entities, match);
    kjFree(match);                                    // free the malloc store node
    resultsV[i] = DB_OK;
    anyOk       = true;
  }

  return anyOk ? DB_OK : DB_ERR;
}

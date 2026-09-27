//
// FILE            corDbEntityAttrsSet.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                                 // bool
#include <string.h>                                   // strcmp

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corNgsild/ldEntityAttrsSet.h"                // ldEntityAttrsSet

#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND
#include "currentState/corDB/corDbIndex.h"        // corDbIndexLookup
#include "currentState/corDB/corDbStore.h"          // corDbEntities
#include "currentState/corDB/corDbEntityAttrsSet.h" // Own interface



// -----------------------------------------------------------------------------
//
// corDbEntityAttrsSet -
//
int corDbEntityAttrsSet(Tenant* tenantP, const char* entityId,
                        CorNode* fragmentDb, bool overwriteScope,
                        uint64_t ts, LdMergeReport* reportP)
{
  COR_DB_WRITE(tenantP);

  CorNode* entities = corDbEntities(tenantP);
  if (entities == NULL)
    return DB_NOT_FOUND;

  //
  // One hop via the id index instead of a walk of the whole store with a
  // corTreeLookup per entity. The loop shape is kept so the body below is unchanged:
  // indexed, it runs exactly once for the hit and not at all for a miss;
  // unindexed - a store that predates the index - it walks as it always did.
  //
  CorDbStore* idxStoreP = corDbStoreOf(tenantP);
  CorNode*    idxHitP   = corDbIndexLookup(idxStoreP, entityId);
  bool        indexed   = (idxStoreP != NULL) && (idxStoreP->idIndex != NULL);

  for (CorNode* eP = indexed ? idxHitP : entities->value.head;
       eP != NULL;
       eP = indexed ? NULL : eP->next)
  {
    CorNode* idP = corTreeLookup(eP, "id");
    if (idP != NULL && idP->type == CorString && strcmp(idP->value.s, entityId) == 0)
    {
      // NULL allocator → malloc heap (tenant store lifetime)
      ldEntityAttrsSet(eP, fragmentDb, overwriteScope, ts, reportP, NULL);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

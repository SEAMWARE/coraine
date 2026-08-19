//
// FILE            ramdbEntityAttrsSet.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <string.h>                                   // strcmp

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjLookup.h"                           // kjLookup

#include "corNgsild/ldEntityAttrsSet.h"                // ldEntityAttrsSet

#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND
#include "currentState/corRamDB/ramdbStore.h"          // ramdbEntities
#include "currentState/corRamDB/ramdbEntityAttrsSet.h" // Own interface



// -----------------------------------------------------------------------------
//
// ramdbEntityAttrsSet -
//
int ramdbEntityAttrsSet(Tenant* tenantP, const char* entityId,
                        KjNode* fragmentDb, bool overwriteScope,
                        uint64_t ts, LdMergeReport* reportP)
{
  KjNode* entities = ramdbEntities(tenantP);
  if (entities == NULL)
    return DB_NOT_FOUND;

  for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
  {
    KjNode* idP = kjLookup(eP, "id");
    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, entityId) == 0)
    {
      // NULL allocator → malloc heap (tenant store lifetime)
      ldEntityAttrsSet(eP, fragmentDb, overwriteScope, ts, reportP, NULL);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

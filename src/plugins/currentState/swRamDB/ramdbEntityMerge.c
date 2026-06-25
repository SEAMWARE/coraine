//
// FILE            ramdbEntityMerge.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// swRamDB entityMerge: locate the live entity node and let ldEntityMerge mutate
// the stored tree in place. No cloning, no wholesale replacement — a PATCH that
// touches one attribute on a 2000-attribute entity only walks the attributes the
// fragment actually names.
//
// The tenant store uses a malloc-backed allocator (NULL Kjson*), so any node
// grafted into target must also be on the malloc heap. We pass NULL as
// targetAllocP to ldEntityMerge.
//

#include <string.h>                                   // strcmp

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjLookup.h"                           // kjLookup

#include "swNgsild/ldEntityMerge.h"                   // ldEntityMerge, LdMergeReport

#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, Tenant
#include "currentState/swRamDB/ramdbStore.h"          // ramdbEntities
#include "currentState/swRamDB/ramdbEntityMerge.h"    // Own interface



// -----------------------------------------------------------------------------
//
// ramdbEntityMergeOne -
//
int ramdbEntityMergeOne(KjNode* entitiesP, const char* entityId,
                        KjNode* fragmentDb, uint64_t ts,
                        LdMergeReport* reportP)
{
  for (KjNode* eP = entitiesP->value.firstChildP; eP != NULL; eP = eP->next)
  {
    KjNode* idP = kjLookup(eP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, entityId) == 0)
    {
      // NULL allocator == malloc heap == tenant store lifetime
      ldEntityFragmentApply(eP, fragmentDb, reportP, ts, NULL);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}



// -----------------------------------------------------------------------------
//
// ramdbEntityMerge -
//
int ramdbEntityMerge(Tenant* tenantP, const char* entityId,
                     KjNode* fragmentDb, uint64_t ts,
                     LdMergeReport* reportP)
{
  KjNode* entities = ramdbEntities(tenantP);
  return ramdbEntityMergeOne(entities, entityId, fragmentDb, ts, reportP);
}

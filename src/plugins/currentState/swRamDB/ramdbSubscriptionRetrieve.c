//
// FILE            ramdbSubscriptionRetrieve.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <string.h>                                   // strcmp

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjLookup.h"                           // kjLookup

#include "swRest/SwRestState.h"                       // swRest
#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, Tenant
#include "currentState/swRamDB/ramdbStore.h"          // ramdbSubscriptions
#include "currentState/swRamDB/ramdbSubscriptionRetrieve.h"  // Own interface



// -----------------------------------------------------------------------------
//
// ramdbSubscriptionRetrieve -
//
int ramdbSubscriptionRetrieve(Tenant* tenantP, const char* subId, KjNode** subPP)
{
  KjNode* subscriptions = ramdbSubscriptions(tenantP);

  for (KjNode* sP = subscriptions->value.firstChildP; sP != NULL; sP = sP->next)
  {
    KjNode* idP = kjLookup(sP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, subId) == 0)
    {
      *subPP = kjClone(swRest.kjsonP, sP);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

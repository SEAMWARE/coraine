//
// FILE            ramdbSubscriptionDelete.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <string.h>                                   // strcmp

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjChildRemove
#include "kjson/kjLookup.h"                           // kjLookup

#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, Tenant
#include "currentState/swRamDB/ramdbStore.h"          // ramdbSubscriptions
#include "currentState/swRamDB/ramdbSubscriptionDelete.h"  // Own interface



// -----------------------------------------------------------------------------
//
// ramdbSubscriptionDelete -
//
int ramdbSubscriptionDelete(Tenant* tenantP, const char* subId)
{
  KjNode* subscriptions = ramdbSubscriptions(tenantP);

  for (KjNode* sP = subscriptions->value.firstChildP; sP != NULL; sP = sP->next)
  {
    KjNode* idP = kjLookup(sP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, subId) == 0)
    {
      kjChildRemove(subscriptions, sP);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

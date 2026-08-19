//
// FILE            ramdbSubscriptionDelete.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                   // strcmp

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjChildRemove
#include "kjson/kjLookup.h"                           // kjLookup

#include "kjson/kjFree.h"                             // kjFree
#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, Tenant
#include "currentState/corRamDB/ramdbStore.h"          // ramdbSubscriptions
#include "currentState/corRamDB/ramdbSubscriptionDelete.h"  // Own interface



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
      kjFree(sP);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

//
// FILE            corDbSubscriptionCreate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                   // strcmp

#include "ktrace/kTrace.h"                            // KT_E
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjBuilder.h"                          // kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup

#include "db/DbDriver.h"                              // DB_OK, DB_ALREADY_EXISTS, DB_ERR, Tenant
#include "currentState/corDB/corDbStore.h"          // corDbSubscriptions
#include "currentState/corDB/corDbSubscriptionCreate.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbSubscriptionCreate -
//
int corDbSubscriptionCreate(Tenant* tenantP, const char* subId, KjNode* subP)
{
  KjNode* subscriptions = corDbSubscriptions(tenantP);

  //
  // Check for duplicate
  //
  for (KjNode* sP = subscriptions->value.firstChildP; sP != NULL; sP = sP->next)
  {
    KjNode* idP = kjLookup(sP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, subId) == 0)
      return DB_ALREADY_EXISTS;
  }

  //
  // Deep-clone the subscription tree (using malloc, not a buffer allocator)
  //
  KjNode* cloneP = kjClone(NULL, subP);
  if (cloneP == NULL)
  {
    KT_E("corDB: kjClone failed for subscription '%s'", subId);
    return DB_ERR;
  }

  kjChildAdd(subscriptions, cloneP);

  return DB_OK;
}

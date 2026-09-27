//
// FILE            corDbSubscriptionCreate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                   // strcmp

#include "corLog/corLog.h"                            // COR_E
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeBuilder.h"                   // corTreeChildAdd
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "db/DbDriver.h"                              // DB_OK, DB_ALREADY_EXISTS, DB_ERR, Tenant
#include "currentState/corDB/corDbStore.h"          // corDbSubscriptions
#include "currentState/corDB/corDbSubscriptionCreate.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbSubscriptionCreate -
//
int corDbSubscriptionCreate(Tenant* tenantP, const char* subId, CorNode* subP)
{
  COR_DB_WRITE(tenantP);

  CorNode* subscriptions = corDbSubscriptions(tenantP);

  //
  // Check for duplicate
  //
  for (CorNode* sP = subscriptions->value.head; sP != NULL; sP = sP->next)
  {
    CorNode* idP = corTreeLookup(sP, "id");

    if (idP != NULL && idP->type == CorString && strcmp(idP->value.s, subId) == 0)
      return DB_ALREADY_EXISTS;
  }

  //
  // Deep-clone the subscription tree (using malloc, not a buffer allocator)
  //
  CorNode* cloneP = corTreeClone(NULL, subP);
  if (cloneP == NULL)
  {
    COR_E("corDB: corTreeClone failed for subscription '%s'", subId);
    return DB_ERR;
  }

  corTreeChildAdd(subscriptions, cloneP);

  return DB_OK;
}

//
// FILE            corDbSubscriptionDelete.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                   // strcmp

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeChildRemove
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corTree/corTreeFree.h"                      // corTreeFree
#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, Tenant
#include "currentState/corDB/corDbStore.h"          // corDbSubscriptions
#include "currentState/corDB/corDbSubscriptionDelete.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbSubscriptionDelete -
//
int corDbSubscriptionDelete(Tenant* tenantP, const char* subId)
{
  COR_DB_WRITE(tenantP);

  CorNode* subscriptions = corDbSubscriptions(tenantP);

  for (CorNode* sP = subscriptions->value.firstChildP; sP != NULL; sP = sP->next)
  {
    CorNode* idP = corTreeLookup(sP, "id");

    if (idP != NULL && idP->type == CorString && strcmp(idP->value.s, subId) == 0)
    {
      corTreeChildRemove(subscriptions, sP);
      corTreeFree(sP);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

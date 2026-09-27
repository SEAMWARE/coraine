//
// FILE            corDbSubscriptionRetrieve.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                   // strcmp

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corRest/CorRestState.h"                       // corRest
#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, Tenant
#include "currentState/corDB/corDbStore.h"          // corDbSubscriptions
#include "currentState/corDB/corDbSubscriptionRetrieve.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbSubscriptionRetrieve -
//
int corDbSubscriptionRetrieve(Tenant* tenantP, const char* subId, CorNode** subPP)
{
  COR_DB_READ(tenantP);

  CorNode* subscriptions = corDbSubscriptions(tenantP);

  for (CorNode* sP = subscriptions->value.head; sP != NULL; sP = sP->next)
  {
    CorNode* idP = corTreeLookup(sP, "id");

    if (idP != NULL && idP->type == CorString && strcmp(idP->value.s, subId) == 0)
    {
      *subPP = corTreeClone(corRest.kallocP, sP);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

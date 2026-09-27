//
// FILE            corDbSubscriptionQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                   // NULL

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeBuilder.h"                   // corTreeArray, corTreeChildAdd
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corRest/CorRestState.h"                       // corRest

#include "db/DbDriver.h"                              // DB_OK, Tenant
#include "currentState/corDB/corDbStore.h"          // corDbSubscriptions
#include "currentState/corDB/corDbSubscriptionQuery.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbSubscriptionQuery -
//
int corDbSubscriptionQuery(Tenant* tenantP, int limit, int offset, CorNode** arrayPP)
{
  COR_DB_READ(tenantP);

  CorNode* subscriptions = corDbSubscriptions(tenantP);
  // Request-arena array (freed at request end / after cache-load), matching
  // mongoc — a NULL (malloc) array would leak its container on every load.
  CorNode* resultArray  = corTreeArray(corRest.kallocP, NULL);

  int ix    = 0;
  int added = 0;

  for (CorNode* sP = subscriptions->value.head; sP != NULL; sP = sP->next)
  {
    if (ix < offset)
    {
      ix++;
      continue;
    }

    if (limit > 0 && added >= limit)
      break;

    CorNode* cloneP = corTreeClone(corRest.kallocP, sP);
    corTreeChildAdd(resultArray, cloneP);
    added++;
    ix++;
  }

  *arrayPP = resultArray;
  return DB_OK;
}

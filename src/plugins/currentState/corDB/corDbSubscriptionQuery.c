//
// FILE            corDbSubscriptionQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                   // NULL

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjBuilder.h"                          // kjArray, kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup
#include "corRest/CorRestState.h"                       // corRest

#include "db/DbDriver.h"                              // DB_OK, Tenant
#include "currentState/corDB/corDbStore.h"          // corDbSubscriptions
#include "currentState/corDB/corDbSubscriptionQuery.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbSubscriptionQuery -
//
int corDbSubscriptionQuery(Tenant* tenantP, int limit, int offset, KjNode** arrayPP)
{
  KjNode* subscriptions = corDbSubscriptions(tenantP);
  // Request-arena array (freed at request end / after cache-load), matching
  // mongoc — a NULL (malloc) array would leak its container on every load.
  KjNode* resultArray   = kjArray(corRest.kjsonP, NULL);

  int ix    = 0;
  int added = 0;

  for (KjNode* sP = subscriptions->value.firstChildP; sP != NULL; sP = sP->next)
  {
    if (ix < offset)
    {
      ix++;
      continue;
    }

    if (limit > 0 && added >= limit)
      break;

    KjNode* cloneP = kjClone(corRest.kjsonP, sP);
    kjChildAdd(resultArray, cloneP);
    added++;
    ix++;
  }

  *arrayPP = resultArray;
  return DB_OK;
}

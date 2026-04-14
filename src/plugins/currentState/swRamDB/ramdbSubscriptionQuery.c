//
// FILE            ramdbSubscriptionQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stddef.h>                                   // NULL

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjBuilder.h"                          // kjArray, kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup

#include "db/DbDriver.h"                              // DB_OK, Tenant
#include "currentState/swRamDB/ramdbStore.h"          // ramdbSubscriptions
#include "currentState/swRamDB/ramdbSubscriptionQuery.h"  // Own interface



// -----------------------------------------------------------------------------
//
// ramdbSubscriptionQuery -
//
int ramdbSubscriptionQuery(Tenant* tenantP, int limit, int offset, KjNode** arrayPP)
{
  KjNode* subscriptions = ramdbSubscriptions(tenantP);
  KjNode* resultArray   = kjArray(NULL, NULL);

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

    KjNode* cloneP = kjClone(NULL, sP);
    kjChildAdd(resultArray, cloneP);
    added++;
    ix++;
  }

  *arrayPP = resultArray;
  return DB_OK;
}

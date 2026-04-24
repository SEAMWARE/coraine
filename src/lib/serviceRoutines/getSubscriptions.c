//
// FILE            getSubscriptions.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/kjBuilder.h"                         // kjArray, kjChildAdd
#include "kjson/kjClone.h"                           // kjClone

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldContextResolve
#include "swNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem
#include "swNgsild/LdPernotCache.h"                  // LdPernotCache, LdPernotItem
#include "swNgsild/ldSubscriptionCompactQ.h"         // ldSubscriptionCompactQ
#include "swNgsild/ldSubscriptionCounters.h"         // ldSubscriptionCountersInject

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getSubscriptions.h"        // Own interface



// -----------------------------------------------------------------------------
//
// getSubscriptions -
//
// Iterate the in-memory sub cache (regular subs) and pernot cache
// (timeInterval-driven subs) together. Counters come live from the cache
// item — so there's no DB round-trip AND stats are always up to date.
//
bool getSubscriptions(void)
{
  ldContextResolve();

  Tenant*        tenantP   = (Tenant*) swNgsild.tenantP;
  LdSubCache*    scP       = (tenantP != NULL) ? (LdSubCache*)    tenantP->subCacheP    : NULL;
  LdPernotCache* pcP       = (tenantP != NULL) ? (LdPernotCache*) tenantP->pernotCacheP : NULL;

  KjNode* arrayP = kjArray(swRest.kjsonP, NULL);

  int skip  = (swNgsild.offset > 0) ? swNgsild.offset : 0;
  int limit = (swNgsild.limit  > 0) ? swNgsild.limit  : -1;  // -1 → unbounded
  int seen  = 0;
  int taken = 0;

  if (scP != NULL)
  {
    for (LdSubCacheItem* it = scP->itemList; it != NULL && (limit < 0 || taken < limit); it = it->next)
    {
      if (it->subTree == NULL) continue;
      if (seen++ < skip)       continue;

      KjNode* subP = kjClone(swRest.kjsonP, it->subTree);
      ldSubscriptionCompactQ(subP, it->qExpr, swNgsild.contextP, &swRest.kalloc);
      ldSubscriptionCountersInject(subP, it);
      kjChildAdd(arrayP, subP);
      taken++;
    }
  }

  if (pcP != NULL)
  {
    for (LdPernotItem* it = pcP->head; it != NULL && (limit < 0 || taken < limit); it = it->next)
    {
      if (it->subTree == NULL) continue;
      if (seen++ < skip)       continue;

      KjNode* subP = kjClone(swRest.kjsonP, it->subTree);
      ldSubscriptionCompactQ(subP, it->qExpr, swNgsild.contextP, &swRest.kalloc);
      ldPernotCountersInject(subP, it);
      kjChildAdd(arrayP, subP);
      taken++;
    }
  }

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = arrayP;
  return true;
}

//
// FILE            getCsourceSubscriptions.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/csourceSubscriptions  (NGSI-LD § 5.11.5)
//
// Cache-only query — no DB persistence in v1. Iterates the tenant's
// regSubCache and returns an array of subscription trees.
//
#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjString, kjChildAdd, kjChildRemove
#include "kjson/kjClone.h"                           // kjClone
#include "kjson/kjLookup.h"                          // kjLookup

#include "swNgsild/swNgsild.h"                       // ldContextResolve, swNgsild
#include "swNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem
#include "swNgsild/ldSubscriptionCompactQ.h"         // ldSubscriptionCompactQ
#include "swNgsild/ldSubscriptionCounters.h"         // ldSubscriptionCountersInject
#include "swNgsild/ldPagination.h"                   // ldPaginationLinkHeader

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getCsourceSubscriptions.h" // Own interface



bool getCsourceSubscriptions(void)
{
  Tenant*     tenantP = (Tenant*) swNgsild.tenantP;
  LdSubCache* cacheP  = (LdSubCache*) tenantP->regSubCacheP;

  ldContextResolve();

  KjNode* arrayP  = kjArray(swRest.kjsonP, NULL);
  bool    hasMore = false;

  if (cacheP != NULL)
  {
    int skip  = (swNgsild.offset > 0) ? swNgsild.offset : 0;
    int limit = (swNgsild.limit  > 0) ? swNgsild.limit  : 1 << 30;
    int idx   = 0;

    for (LdSubCacheItem* itemP = cacheP->itemList; itemP != NULL; itemP = itemP->next)
    {
      if (idx < skip) { ++idx; continue; }
      if ((idx - skip) >= limit) { hasMore = true; break; }
      ++idx;

      if (itemP->subTree == NULL)
        continue;

      KjNode* subP = kjClone(swRest.kjsonP, itemP->subTree);
      ldSubscriptionCompactQ(subP, itemP->qExpr, swNgsild.contextP, &swRest.kalloc);
      ldSubscriptionCountersInject(subP, itemP);

      // Hide the internal marker
      KjNode* kindP = kjLookup(subP, "_subKind");
      if (kindP != NULL)
        kjChildRemove(subP, kindP);

      // § 5.2 Subscription table: `notificationTrigger` is "not applicable
      // and shall be ignored" for CSR-sub. The lib's ldCheckSubscription
      // strips it on create/update, so the field is never stored — no
      // default-emit here (entity-sub list in getSubscriptions.c keeps it).

      // Strip the broker-internal `_jcResolved` so the response only carries
      // user-provided `jsonldContext` (if any).
      KjNode* jcP = kjLookup(subP, "_jcResolved");
      if (jcP != NULL)
        kjChildRemove(subP, jcP);

      kjChildAdd(arrayP, subP);
    }
  }

  ldPaginationLinkHeader(hasMore);

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = arrayP;
  return true;
}

//
// FILE            getSubscriptions.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                  // NULL
#include <stdio.h>                                   // snprintf

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/corRestOutHeader.h"                  // corRestOutHeaderAdd
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjString, kjChildAdd
#include "kjson/kjClone.h"                           // kjClone
#include "kjson/kjLookup.h"                          // kjLookup

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild, ldContextResolve
#include "corNgsild/ldStripSysAttrs.h"                // ldStripSysAttrs
#include "corNgsild/ldSysTimestamp.h"                 // ldSysTimestampsToIso
#include "corNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem
#include "corNgsild/LdPernotCache.h"                  // LdPernotCache, LdPernotItem
#include "corNgsild/ldSubscriptionCompactQ.h"         // ldSubscriptionCompactQ
#include "corNgsild/ldPagination.h"                   // ldPaginationLinkHeader
#include "corNgsild/ldSubscriptionCounters.h"         // ldSubscriptionCountersInject

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getSubscriptions.h"        // Own interface



// -----------------------------------------------------------------------------
//
// subPostProcess - mirror of getSubscription's response-shaping steps:
//   · default-emit notificationTrigger (§ 5.2.12)
//   · `jsonldContext` is spec-visible and passes through untouched
//
static void subPostProcess(KjNode* subP)
{
  if (kjLookup(subP, "notificationTrigger") == NULL)
  {
    KjNode* trigArr = kjArray(corRest.kjsonP, "notificationTrigger");
    kjChildAdd(trigArr, kjString(corRest.kjsonP, NULL, "attributeCreated"));
    kjChildAdd(trigArr, kjString(corRest.kjsonP, NULL, "attributeUpdated"));
    kjChildAdd(subP, trigArr);
  }

  //
  // `jsonldContext` is spec-visible on a Subscription (§ 10.5.2.4) and is no
  // longer hidden behind a broker-internal name - nothing to strip.
  //

  // § 6.4.5 — createdAt/modifiedAt (stored as nanosecond integers) are rendered
  // as ISO 8601 only when the client asks for system attributes; stripped otherwise.
  if (corNgsild.sysAttrs == false)
    ldStripSysAttrs(subP);
  else
    ldSysTimestampsToIso(subP, &corRest.kalloc);
}



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

  Tenant*        tenantP   = (Tenant*) corNgsild.tenantP;
  LdSubCache*    scP       = (LdSubCache*)    tenantP->subCacheP   ;
  LdPernotCache* pcP       = (LdPernotCache*) tenantP->pernotCacheP;

  KjNode* arrayP = kjArray(corRest.kjsonP, NULL);

  int skip  = (corNgsild.offset > 0) ? corNgsild.offset : 0;
  // corNgsild.limit defaults to 20 (ldHooks); 0 only when the client explicitly
  // asked for limit=0 — valid only together with count=true (ldParamsValidate),
  // meaning "just the count, no items". Mapping it to unbounded returned the
  // whole set, so keep it as-is: limit=0 → an empty page.
  int limit = corNgsild.limit;
  int seen  = 0;
  int taken = 0;

  // First pass: count total subscriptions (across change-driven sub
  // cache and pernot cache) so the pagination Link header can decide
  // whether a `rel="next"` is needed. § 6.3.10: GET on a collection
  // is paginated; clients need prev/next/first to walk the set.
  int total = 0;
  if (scP != NULL)
    for (LdSubCacheItem* it = scP->itemList; it != NULL; it = it->next)
      if (it->subTree != NULL) total++;
  if (pcP != NULL)
    for (LdPernotItem* it = pcP->head; it != NULL; it = it->next)
      if (it->subTree != NULL) total++;

  if (scP != NULL)
  {
    for (LdSubCacheItem* it = scP->itemList; it != NULL && (limit < 0 || taken < limit); it = it->next)
    {
      if (it->subTree == NULL) continue;
      if (seen++ < skip)       continue;

      KjNode* subP = kjClone(corRest.kjsonP, it->subTree);
      ldSubscriptionCompactQ(subP, it->qExpr, corNgsild.contextP, &corRest.kalloc);
      ldSubscriptionCountersInject(subP, it);
      subPostProcess(subP);
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

      KjNode* subP = kjClone(corRest.kjsonP, it->subTree);
      ldSubscriptionCompactQ(subP, it->qExpr, corNgsild.contextP, &corRest.kalloc);
      ldPernotCountersInject(subP, it);
      subPostProcess(subP);
      kjChildAdd(arrayP, subP);
      taken++;
    }
  }

  // § 7.4.2.2: prev/next pointers describe iterating the pages of a result set;
  // a page that is empty (none taken) AND has nothing more pending is no
  // iteration, so emit neither; keep next when more pages remain (hasMore).
  bool hasMore = (skip + taken < total);
  if ((taken > 0 || hasMore) && (hasMore || skip > 0))
    ldPaginationLinkHeader(hasMore);

  // § 7.5 / § 6.4.6 (TS 104-176): relay the total element count when requested.
  if (corNgsild.count)
  {
    char* countStr = (char*) kaAlloc(&corRest.kalloc, 32);
    snprintf(countStr, 32, "%d", total);
    corRestOutHeaderAdd("NGSILD-Results-Count", countStr);
  }

  corNgsild.rawResponse    = true;
  corRest.out.responseTree = arrayP;
  return true;
}

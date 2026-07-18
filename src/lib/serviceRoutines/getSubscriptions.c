//
// FILE            getSubscriptions.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stddef.h>                                  // NULL
#include <stdio.h>                                   // snprintf

#include "swRest/SwRestState.h"                      // swRest
#include "swRest/swRestOutHeader.h"                  // swRestOutHeaderAdd
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjString, kjChildAdd
#include "kjson/kjClone.h"                           // kjClone
#include "kjson/kjLookup.h"                          // kjLookup

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldContextResolve
#include "swNgsild/ldStripSysAttrs.h"                // ldStripSysAttrs
#include "swNgsild/ldSysTimestamp.h"                 // ldSysTimestampsToIso
#include "swNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem
#include "swNgsild/LdPernotCache.h"                  // LdPernotCache, LdPernotItem
#include "swNgsild/ldSubscriptionCompactQ.h"         // ldSubscriptionCompactQ
#include "swNgsild/ldPagination.h"                   // ldPaginationLinkHeader
#include "swNgsild/ldSubscriptionCounters.h"         // ldSubscriptionCountersInject

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getSubscriptions.h"        // Own interface



// -----------------------------------------------------------------------------
//
// subPostProcess - mirror of getSubscription's response-shaping steps:
//   · default-emit notificationTrigger (§ 5.2.12)
//   · strip broker-internal `_jcResolved`
//
static void subPostProcess(KjNode* subP)
{
  if (kjLookup(subP, "notificationTrigger") == NULL)
  {
    KjNode* trigArr = kjArray(swRest.kjsonP, "notificationTrigger");
    kjChildAdd(trigArr, kjString(swRest.kjsonP, NULL, "attributeCreated"));
    kjChildAdd(trigArr, kjString(swRest.kjsonP, NULL, "attributeUpdated"));
    kjChildAdd(subP, trigArr);
  }

  KjNode* jcP = kjLookup(subP, "_jcResolved");
  if (jcP != NULL)
    kjChildRemove(subP, jcP);

  // § 6.4.5 — createdAt/modifiedAt (stored as nanosecond integers) are rendered
  // as ISO 8601 only when the client asks for system attributes; stripped otherwise.
  if (swNgsild.sysAttrs == false)
    ldStripSysAttrs(subP);
  else
    ldSysTimestampsToIso(subP, &swRest.kalloc);
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

  Tenant*        tenantP   = (Tenant*) swNgsild.tenantP;
  LdSubCache*    scP       = (LdSubCache*)    tenantP->subCacheP   ;
  LdPernotCache* pcP       = (LdPernotCache*) tenantP->pernotCacheP;

  KjNode* arrayP = kjArray(swRest.kjsonP, NULL);

  int skip  = (swNgsild.offset > 0) ? swNgsild.offset : 0;
  // swNgsild.limit defaults to 20 (ldHooks); 0 only when the client explicitly
  // asked for limit=0 — valid only together with count=true (ldParamsValidate),
  // meaning "just the count, no items". Mapping it to unbounded returned the
  // whole set, so keep it as-is: limit=0 → an empty page.
  int limit = swNgsild.limit;
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

      KjNode* subP = kjClone(swRest.kjsonP, it->subTree);
      ldSubscriptionCompactQ(subP, it->qExpr, swNgsild.contextP, &swRest.kalloc);
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

      KjNode* subP = kjClone(swRest.kjsonP, it->subTree);
      ldSubscriptionCompactQ(subP, it->qExpr, swNgsild.contextP, &swRest.kalloc);
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
  if (swNgsild.count)
  {
    char* countStr = (char*) kaAlloc(&swRest.kalloc, 32);
    snprintf(countStr, 32, "%d", total);
    swRestOutHeaderAdd("NGSILD-Results-Count", countStr);
  }

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = arrayP;
  return true;
}

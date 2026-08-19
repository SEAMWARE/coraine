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
#include <stdio.h>                                   // snprintf

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/corRestOutHeader.h"                  // corRestOutHeaderAdd
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjString, kjChildAdd, kjChildRemove
#include "kjson/kjClone.h"                           // kjClone
#include "kjson/kjLookup.h"                          // kjLookup

#include "corNgsild/corNgsild.h"                       // ldContextResolve, corNgsild
#include "corNgsild/ldStripSysAttrs.h"                // ldStripSysAttrs
#include "corNgsild/ldSysTimestamp.h"                 // ldSysTimestampsToIso
#include "corNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem
#include "corNgsild/ldSubscriptionCompactQ.h"         // ldSubscriptionCompactQ
#include "corNgsild/ldSubscriptionCounters.h"         // ldSubscriptionCountersInject
#include "corNgsild/ldPagination.h"                   // ldPaginationLinkHeader

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getCsourceSubscriptions.h" // Own interface



bool getCsourceSubscriptions(void)
{
  Tenant*     tenantP = (Tenant*) corNgsild.tenantP;
  LdSubCache* cacheP  = (LdSubCache*) tenantP->regSubCacheP;

  ldContextResolve();

  KjNode* arrayP  = kjArray(corRest.kjsonP, NULL);
  bool    hasMore = false;

  // Total across the CSR-subscription cache — for the count header and to
  // decide whether a further page is pending.
  int total = 0;
  if (cacheP != NULL)
    for (LdSubCacheItem* it = cacheP->itemList; it != NULL; it = it->next)
      if (it->subTree != NULL) total++;

  if (cacheP != NULL)
  {
    int skip  = (corNgsild.offset > 0) ? corNgsild.offset : 0;
    // corNgsild.limit defaults to 20 (ldHooks); 0 only when the client asked for
    // limit=0 (valid only with count=true) — "just the count, no items". Keep
    // it as-is so limit=0 yields an empty page, not the whole set.
    int limit = corNgsild.limit;
    int idx   = 0;

    for (LdSubCacheItem* itemP = cacheP->itemList; itemP != NULL; itemP = itemP->next)
    {
      if (idx < skip) { ++idx; continue; }
      if ((idx - skip) >= limit) { hasMore = true; break; }
      ++idx;

      if (itemP->subTree == NULL)
        continue;

      KjNode* subP = kjClone(corRest.kjsonP, itemP->subTree);
      ldSubscriptionCompactQ(subP, itemP->qExpr, corNgsild.contextP, &corRest.kalloc);
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

      // § 6.4.5 — createdAt/modifiedAt (nanosecond integers) → ISO 8601 under sysAttrs; stripped otherwise.
      if (corNgsild.sysAttrs == false)
        ldStripSysAttrs(subP);
      else
        ldSysTimestampsToIso(subP, &corRest.kalloc);

      kjChildAdd(arrayP, subP);
    }
  }

  // § 7.4.2.2: no prev/next pointers for a page that is empty AND has nothing
  // more pending; keep next when more pages remain (hasMore).
  if (arrayP->value.firstChildP != NULL || hasMore)
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

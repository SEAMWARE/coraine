//
// FILE            getSubscription.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                  // NULL

#include "corRest/CorRestState.h"                      // corRest
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjClone.h"                           // kjClone
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjBuilder.h"                         // kjArray, kjString, kjChildAdd

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild, ldContextResolve
#include "corNgsild/ldStripSysAttrs.h"                // ldStripSysAttrs
#include "corNgsild/ldSysTimestamp.h"                 // ldSysTimestampsToIso
#include "corNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem
#include "corNgsild/ldSubCache.h"                     // ldSubCacheItemLookup
#include "corNgsild/LdPernotCache.h"                  // LdPernotCache, LdPernotItem
#include "corNgsild/ldPernotCache.h"                  // ldPernotCacheItemLookup
#include "corNgsild/ldSubscriptionCompactQ.h"         // ldSubscriptionCompactQ
#include "corNgsild/ldSubscriptionCounters.h"         // ldSubscriptionCountersInject

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getSubscription.h"         // Own interface



// -----------------------------------------------------------------------------
//
// getSubscription -
//
// Read straight from the sub / pernot cache — cache holds the full subTree
// plus live counters. Cloning into the request arena so the response path
// can compact IRIs and add stats without mutating cache.
//
bool getSubscription(void)
{
  const char* subId = corRest.in.wildcard[0];

  Tenant*         tenantP    = (Tenant*) corNgsild.tenantP;
  LdSubCacheItem* cacheItem  = (tenantP->subCacheP != NULL)    ? ldSubCacheItemLookup((LdSubCache*) tenantP->subCacheP, subId)       : NULL;
  LdPernotItem*   pernotItem = (tenantP->pernotCacheP != NULL) ? ldPernotCacheItemLookup((LdPernotCache*) tenantP->pernotCacheP, subId) : NULL;
  KjNode*         srcTree    = (cacheItem != NULL) ? cacheItem->subTree : (pernotItem != NULL) ? pernotItem->subTree : NULL;

  if (srcTree == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "subscription '%s' not found", subId);
    return true;
  }

  ldContextResolve();

  KjNode*  subP  = kjClone(corRest.kjsonP, srcTree);
  LdQNode* qExpr = (cacheItem != NULL) ? cacheItem->qExpr : (pernotItem != NULL) ? pernotItem->qExpr : NULL;
  ldSubscriptionCompactQ(subP, qExpr, corNgsild.contextP, &corRest.kalloc);

  // § 5.2.12: notificationTrigger defaults to ["attributeCreated",
  // "attributeUpdated"] when not specified. Surface the active default
  // in the response so clients see what the subscription will actually
  // trigger on, rather than silently inheriting an undocumented value.
  if (kjLookup(subP, "notificationTrigger") == NULL)
  {
    KjNode* trigArr = kjArray(corRest.kjsonP, "notificationTrigger");
    kjChildAdd(trigArr, kjString(corRest.kjsonP, NULL, "attributeCreated"));
    kjChildAdd(trigArr, kjString(corRest.kjsonP, NULL, "attributeUpdated"));
    kjChildAdd(subP, trigArr);
  }

  // `_jcResolved` is the broker-filled @context URL used internally for
  // notification compaction when the user didn't supply `jsonldContext`.
  // Strip it from the response so we only emit the spec-visible field.
  KjNode* jcP = kjLookup(subP, "_jcResolved");
  if (jcP != NULL)
    kjChildRemove(subP, jcP);

  if (cacheItem != NULL) ldSubscriptionCountersInject(subP, cacheItem);
  else                   ldPernotCountersInject(subP, pernotItem);

  // § 6.4.5 — createdAt/modifiedAt (nanosecond integers) → ISO 8601 only under
  // sysAttrs; stripped otherwise.
  if (corNgsild.sysAttrs == false)
    ldStripSysAttrs(subP);
  else
    ldSysTimestampsToIso(subP, &corRest.kalloc);

  corNgsild.rawResponse    = true;
  corRest.out.responseTree = subP;
  return true;
}

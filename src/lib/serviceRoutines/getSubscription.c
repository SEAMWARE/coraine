//
// FILE            getSubscription.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjClone.h"                           // kjClone

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldContextResolve
#include "swNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem
#include "swNgsild/ldSubCache.h"                     // ldSubCacheItemLookup
#include "swNgsild/LdPernotCache.h"                  // LdPernotCache, LdPernotItem
#include "swNgsild/ldPernotCache.h"                  // ldPernotCacheItemLookup
#include "swNgsild/ldSubscriptionCompactQ.h"         // ldSubscriptionCompactQ
#include "swNgsild/ldSubscriptionCounters.h"         // ldSubscriptionCountersInject

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
  const char* subId = swRest.in.wildcard[0];

  Tenant*         tenantP    = (Tenant*) swNgsild.tenantP;
  LdSubCacheItem* cacheItem  = (tenantP->subCacheP != NULL)    ? ldSubCacheItemLookup((LdSubCache*) tenantP->subCacheP, subId)       : NULL;
  LdPernotItem*   pernotItem = (tenantP->pernotCacheP != NULL) ? ldPernotCacheItemLookup((LdPernotCache*) tenantP->pernotCacheP, subId) : NULL;
  KjNode*         srcTree    = (cacheItem != NULL) ? cacheItem->subTree : (pernotItem != NULL) ? pernotItem->subTree : NULL;

  if (srcTree == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "subscription '%s' not found", subId);
    return true;
  }

  ldContextResolve();

  KjNode*  subP  = kjClone(swRest.kjsonP, srcTree);
  LdQNode* qExpr = (cacheItem != NULL) ? cacheItem->qExpr : (pernotItem != NULL) ? pernotItem->qExpr : NULL;
  ldSubscriptionCompactQ(subP, qExpr, swNgsild.contextP, &swRest.kalloc);

  if (cacheItem != NULL) ldSubscriptionCountersInject(subP, cacheItem);
  else                   ldPernotCountersInject(subP, pernotItem);

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = subP;
  return true;
}

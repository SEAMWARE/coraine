//
// FILE            getSubscription.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stddef.h>                                  // NULL

#include "corRest/CorRestState.h"                      // corRest
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeClone.h"                    // corTreeClone
#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corTree/corTreeBuilder.h"                  // corTreeArray, corTreeString, corTreeChildAdd

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild, ldContextResolve
#include "corNgsild/ldStripSysAttrs.h"                // ldStripSysAttrs
#include "corNgsild/ldSysTimestamp.h"                 // ldSysTimestampsToIso
#include "corNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem
#include "corNgsild/ldSubCache.h"                     // ldSubCacheItemLookup
#include "corNgsild/LdPernotCache.h"                  // LdPernotCache, LdPernotItem
#include "corNgsild/ldPernotCache.h"                  // ldPernotCacheItemLookupPinned, ldPernotCacheItemUnpin
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
static bool subscriptionRender(const char* subId, LdSubCacheItem* cacheItem, LdPernotItem* pernotItem);

bool getSubscription(void)
{
  const char* subId = corRest.in.wildcard[0];

  Tenant*         tenantP    = (Tenant*) corNgsild.tenantP;
  LdSubCache*     subCacheP  = (LdSubCache*) tenantP->subCacheP;
  LdSubCacheItem* cacheItem  = NULL;

  //
  // Looked up under the rdlock and PINNED: the item is read to the end of this function (its
  // tree, qExpr and counters), across ldContextResolve - which may download a context - and a
  // DELETE of the subscription meanwhile freed it. The lookup had no lock at all.
  //
  if (subCacheP != NULL)
  {
    ldSubCacheRdLock(subCacheP);
    cacheItem = ldSubCacheItemLookup(subCacheP, subId);
    if (cacheItem != NULL)
      ldSubCacheItemPin(cacheItem);
    ldSubCacheUnlock(subCacheP);
  }

  // Not a regular subscription? A periodic one, from the pernot cache - pinned the same way
  LdPernotItem* pernotItem = NULL;

  if ((cacheItem == NULL) && (tenantP->pernotCacheP != NULL))
    pernotItem = ldPernotCacheItemLookupPinned((LdPernotCache*) tenantP->pernotCacheP, subId);

  bool r = subscriptionRender(subId, cacheItem, pernotItem);

  if (cacheItem != NULL)
    ldSubCacheItemUnpin(cacheItem);
  if (pernotItem != NULL)
    ldPernotCacheItemUnpin(pernotItem);

  return r;
}



// -----------------------------------------------------------------------------
//
// subscriptionRender - the response, from a PINNED item of the sub cache or of the pernot cache
//
static bool subscriptionRender(const char* subId, LdSubCacheItem* cacheItem, LdPernotItem* pernotItem)
{
  CorNode*        srcTree    = (cacheItem != NULL) ? cacheItem->subTree : (pernotItem != NULL) ? pernotItem->subTree : NULL;

  if (srcTree == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "subscription '%s' not found", subId);
    return true;
  }

  ldContextResolve();

  CorNode* subP  = corTreeClone(corRest.kallocP, srcTree);
  LdQNode* qExpr = (cacheItem != NULL) ? cacheItem->qExpr : (pernotItem != NULL) ? pernotItem->qExpr : NULL;
  ldSubscriptionCompactQ(subP, qExpr, corNgsild.contextP, &corRest.kalloc);

  // § 5.2.12: notificationTrigger defaults to ["attributeCreated",
  // "attributeUpdated"] when not specified. Surface the active default
  // in the response so clients see what the subscription will actually
  // trigger on, rather than silently inheriting an undocumented value.
  if (corTreeLookup(subP, "notificationTrigger") == NULL)
  {
    CorNode* trigArr = corTreeArray(corRest.kallocP, "notificationTrigger");
    corTreeChildAdd(trigArr, corTreeString(corRest.kallocP, NULL, "attributeCreated"));
    corTreeChildAdd(trigArr, corTreeString(corRest.kallocP, NULL, "attributeUpdated"));
    corTreeChildAdd(subP, trigArr);
  }

  //
  // No `_jcResolved` to strip any more: § 10.5.2.4 says the field SHALL be
  // initialized, so the create path writes the resolved URL under its spec
  // name and it belongs in the response. (The CSR side still uses the
  // internal name - the spec has no such clause there, spec-doubts #16.)
  //

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

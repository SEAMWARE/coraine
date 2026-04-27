//
// FILE            getCsourceSubscription.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/csourceSubscriptions/{id}  (NGSI-LD § 5.11.4)
//
#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjChildRemove
#include "kjson/kjClone.h"                           // kjClone
#include "kjson/kjLookup.h"                          // kjLookup

#include "swNgsild/swNgsild.h"                       // ldError, ldContextResolve, swNgsild
#include "swNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem
#include "swNgsild/ldSubCache.h"                     // ldSubCacheItemLookup
#include "swNgsild/ldSubscriptionCompactQ.h"         // ldSubscriptionCompactQ
#include "swNgsild/ldSubscriptionCounters.h"         // ldSubscriptionCountersInject

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getCsourceSubscription.h"  // Own interface



bool getCsourceSubscription(void)
{
  const char* subId = swRest.in.wildcard[0];

  Tenant*     tenantP = (Tenant*) swNgsild.tenantP;
  LdSubCache* cacheP  = (LdSubCache*) tenantP->regSubCacheP;

  LdSubCacheItem* itemP = (cacheP != NULL) ? ldSubCacheItemLookup(cacheP, subId) : NULL;

  if (itemP == NULL || itemP->subTree == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "CSR subscription '%s' not found", subId);
    return true;
  }

  ldContextResolve();

  KjNode* subP = kjClone(swRest.kjsonP, itemP->subTree);
  ldSubscriptionCompactQ(subP, itemP->qExpr, swNgsild.contextP, &swRest.kalloc);
  ldSubscriptionCountersInject(subP, itemP);

  // Hide the internal marker
  KjNode* kindP = kjLookup(subP, "_subKind");
  if (kindP != NULL)
    kjChildRemove(subP, kindP);

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = subP;
  return true;
}

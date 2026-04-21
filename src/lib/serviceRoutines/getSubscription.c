//
// FILE            getSubscription.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjBuilder.h"                         // kjChildRemove
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldContextResolve
#include "swNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem
#include "swNgsild/ldSubCache.h"                     // ldSubCacheItemLookup
#include "swNgsild/LdPernotCache.h"                  // LdPernotCache, LdPernotItem
#include "swNgsild/ldPernotCache.h"                  // ldPernotCacheItemLookup
#include "swNgsild/ldSubscriptionCompactQ.h"         // ldSubscriptionCompactQ
#include "swNgsild/ldSubscriptionCounters.h"         // ldSubscriptionCountersInject

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getSubscription.h"         // Own interface



// -----------------------------------------------------------------------------
//
// getSubscription -
//
bool getSubscription(void)
{
  const char* subId = swRest.in.wildcard[0];

  if (db.subscriptionRetrieve == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented", "subscription CRUD not supported by this DB plugin");
    return true;
  }

  KjNode* subP = NULL;
  int     r    = db.subscriptionRetrieve((Tenant*) swNgsild.tenantP, subId, &subP);

  if (r == DB_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "subscription '%s' not found", subId);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error retrieving subscription '%s'", subId);
    return true;
  }

  //
  // Hide CSR-subs from this endpoint — they live under /csourceSubscriptions
  //
  KjNode* kindP = kjLookup(subP, "_subKind");
  if (kindP != NULL && kindP->type == KjString && strcmp(kindP->value.s, "csr") == 0)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "subscription '%s' not found", subId);
    return true;
  }
  if (kindP != NULL)
    kjChildRemove(subP, kindP);

  // Resolve @context (may not be set yet for GET with no URL params)
  ldContextResolve();

  // Compact q-filter attr names against the response @context
  Tenant*         tenantP   = (Tenant*) swNgsild.tenantP;
  LdSubCacheItem* cacheItem = (tenantP->subCacheP != NULL)
    ? ldSubCacheItemLookup((LdSubCache*) tenantP->subCacheP, subId)
    : NULL;
  LdPernotItem* pernotItem = (tenantP->pernotCacheP != NULL)
    ? ldPernotCacheItemLookup((LdPernotCache*) tenantP->pernotCacheP, subId)
    : NULL;
  LdQNode* qExpr = (cacheItem != NULL) ? cacheItem->qExpr :
                    (pernotItem != NULL) ? pernotItem->qExpr : NULL;
  ldSubscriptionCompactQ(subP, qExpr, swNgsild.contextP, &swRest.kalloc);

  if (cacheItem != NULL)
    ldSubscriptionCountersInject(subP, cacheItem);
  else if (pernotItem != NULL)
    ldPernotCountersInject(subP, pernotItem);

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = subP;
  return true;
}

//
// FILE            getSubscriptions.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/kjLookup.h"                          // kjLookup
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldContextResolve
#include "swNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem
#include "swNgsild/ldSubCache.h"                     // ldSubCacheItemLookup
#include "swNgsild/ldSubscriptionCompactQ.h"         // ldSubscriptionCompactQ
#include "swNgsild/ldSubscriptionCounters.h"         // ldSubscriptionCountersInject

#include "db/DbDriver.h"                             // db, DB_OK
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getSubscriptions.h"        // Own interface



// -----------------------------------------------------------------------------
//
// getSubscriptions -
//
bool getSubscriptions(void)
{
  if (db.subscriptionQuery == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented", "subscription CRUD not supported by this DB plugin");
    return true;
  }

  KjNode* arrayP = NULL;
  int     r      = db.subscriptionQuery((Tenant*) swNgsild.tenantP, swNgsild.limit, swNgsild.offset, &arrayP);

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error querying subscriptions");
    return true;
  }

  // Compact q-filter attr names in each subscription
  // Resolve @context (may not be set yet for GET with no URL params)
  ldContextResolve();

  Tenant*      tenantP  = (Tenant*) swNgsild.tenantP;
  LdSubCache*  scP      = (LdSubCache*) tenantP->subCacheP;

  for (KjNode* subP = arrayP->value.firstChildP; subP != NULL; subP = subP->next)
  {
    KjNode* idP = kjLookup(subP, "id");
    LdSubCacheItem* cacheItem = (scP != NULL && idP != NULL && idP->type == KjString)
      ? ldSubCacheItemLookup(scP, idP->value.s)
      : NULL;
    LdQNode* qExpr = (cacheItem != NULL) ? cacheItem->qExpr : NULL;
    ldSubscriptionCompactQ(subP, qExpr, swNgsild.contextP, &swRest.kalloc);
    ldSubscriptionCountersInject(subP, cacheItem);
  }

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = arrayP;
  return true;
}

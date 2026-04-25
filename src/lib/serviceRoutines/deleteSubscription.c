//
// FILE            deleteSubscription.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/KjNode.h"                            // KjNode
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem
#include "swNgsild/ldSubCache.h"                     // ldSubCacheItemRemove
#include "swNgsild/LdPernotCache.h"                  // LdPernotCache
#include "swNgsild/ldPernotCache.h"                  // ldPernotCacheItemRemove
#include "swNgsild/LdRegCache.h"                     // LdRegCache
#include "swNgsild/ldDistSub.h"                      // ldDistSubCascadeDelete
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/deleteSubscription.h"      // Own interface



// -----------------------------------------------------------------------------
//
// deleteSubscription -
//
bool deleteSubscription(void)
{
  const char* subId = swRest.in.wildcard[0];

  if (db.subscriptionDelete == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented", "subscription CRUD not supported by this DB plugin");
    return true;
  }

  //
  // Block deletion of CSR-subs via this endpoint — they live under
  // /csourceSubscriptions. Look them up in the CSR cache to avoid a DB
  // round-trip.
  //
  {
    Tenant* _t = (Tenant*) swNgsild.tenantP;
    if (_t != NULL && _t->regSubCacheP != NULL
        && ldSubCacheItemLookup((LdSubCache*) _t->regSubCacheP, subId) != NULL)
    {
      ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "subscription '%s' not found", subId);
      return true;
    }
  }

  int r = db.subscriptionDelete((Tenant*) swNgsild.tenantP, subId);

  if (r == DB_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "subscription '%s' not found", subId);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error deleting subscription '%s'", subId);
    return true;
  }

  //
  // § 5.8.1.4 — DELETE cascade. Walk subordinateP and DELETE each
  // remote derivative before freeing the cache item; failures don't
  // block the local delete.
  //
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  if (tenantP->subCacheP != NULL && tenantP->regCacheP != NULL)
  {
    LdSubCacheItem* itemP = ldSubCacheItemLookup((LdSubCache*) tenantP->subCacheP, subId);
    if (itemP != NULL && itemP->subordinateP != NULL)
    {
      const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);
      ldDistSubCascadeDelete(itemP, (LdRegCache*) tenantP->regCacheP, ownAlias);
    }
  }

  //
  // Remove from subscription cache
  //
  if (tenantP->subCacheP != NULL)
    ldSubCacheItemRemove((LdSubCache*) tenantP->subCacheP, subId);
  if (tenantP->pernotCacheP != NULL)
    ldPernotCacheItemRemove((LdPernotCache*) tenantP->pernotCacheP, subId);

  swRest.out.httpStatusCode = 204;
  return true;
}

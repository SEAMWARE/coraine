//
// FILE            deleteSubscription.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp

#include "corRest/CorRestState.h"                      // corRest
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/KjNode.h"                            // KjNode
#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem
#include "corNgsild/ldSubCache.h"                     // ldSubCacheItemRemove
#include "corNgsild/LdPernotCache.h"                  // LdPernotCache
#include "corNgsild/ldPernotCache.h"                  // ldPernotCacheItemRemove
#include "corNgsild/LdRegCache.h"                     // LdRegCache
#include "corNgsild/ldDistSub.h"                      // ldDistSubCascadeDelete
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/deleteSubscription.h"      // Own interface



// -----------------------------------------------------------------------------
//
// deleteSubscription -
//
bool deleteSubscription(void)
{
  const char* subId = corRest.in.wildcard[0];

  if (db.subscriptionDelete == NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented", "subscription CRUD not supported by this DB plugin");
    return true;
  }

  //
  // Block deletion of CSR-subs via this endpoint — they live under
  // /csourceSubscriptions. Look them up in the CSR cache to avoid a DB
  // round-trip.
  //
  {
    Tenant* _t = (Tenant*) corNgsild.tenantP;
    if (_t != NULL && _t->regSubCacheP != NULL
        && ldSubCacheItemLookup((LdSubCache*) _t->regSubCacheP, subId) != NULL)
    {
      ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "subscription '%s' not found", subId);
      return true;
    }
  }

  int r = db.subscriptionDelete((Tenant*) corNgsild.tenantP, subId);

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
  Tenant*     tenantP   = (Tenant*) corNgsild.tenantP;
  LdSubCache* subCacheP = (LdSubCache*) tenantP->subCacheP;

  // Under the sub wrlock: pin the item (so the cascade can use it after we drop
  // the lock) and retire it from the cache. We do the cascade AFTER releasing
  // the sub lock — it touches the reg cache + does remote DELETEs (I/O), and the
  // lock order is reg-before-sub, so we must never hold the sub lock across it.
  LdSubCacheItem* itemP = NULL;
  ldSubCacheWrLock(subCacheP);
  if (subCacheP != NULL)
  {
    itemP = ldSubCacheItemLookup(subCacheP, subId);
    if (itemP != NULL)
      ldSubCacheItemPin(itemP);
    ldSubCacheItemRemove(subCacheP, subId);   // retires itemP (parked, since pinned)
  }
  ldSubCacheUnlock(subCacheP);

  if (itemP != NULL && itemP->subordinateP != NULL && tenantP->regCacheP != NULL)
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);
    ldDistSubCascadeDelete(itemP, (LdRegCache*) tenantP->regCacheP, ownAlias);
  }
  if (itemP != NULL)
    ldSubCacheItemUnpin(itemP);

  if (tenantP->pernotCacheP != NULL)
    ldPernotCacheItemRemove((LdPernotCache*) tenantP->pernotCacheP, subId);

  corRest.out.httpStatusCode = 204;
  return true;
}

//
// FILE            corDbStore.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                  // NULL

#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjArray, kjChildAdd
#include "kjson/kjLookup.h"                          // kjLookup

#include "db/Tenant.h"                               // Tenant

#include "currentState/corDB/corDbStore.h"         // Own interface



// -----------------------------------------------------------------------------
//
// corDbTenantStore - return (or create) the per-tenant KjNode tree
//
KjNode* corDbTenantStore(Tenant* tenantP)
{
  if (tenantP->pluginData != NULL)
    return (KjNode*) tenantP->pluginData;

  //
  // First access for this tenant — build the store tree using malloc (NULL allocator)
  //
  KjNode* store         = kjObject(NULL, NULL);
  KjNode* entities      = kjArray(NULL, "entities");
  KjNode* subscriptions = kjArray(NULL, "subscriptions");
  KjNode* registrations = kjArray(NULL, "registrations");

  kjChildAdd(store, entities);
  kjChildAdd(store, subscriptions);
  kjChildAdd(store, registrations);

  tenantP->pluginData = store;

  return store;
}



// -----------------------------------------------------------------------------
//
// corDbEntities - return the "entities" array for a tenant
//
KjNode* corDbEntities(Tenant* tenantP)
{
  KjNode* store = corDbTenantStore(tenantP);

  return kjLookup(store, "entities");
}



// -----------------------------------------------------------------------------
//
// corDbSubscriptions - return the "subscriptions" array for a tenant
//
KjNode* corDbSubscriptions(Tenant* tenantP)
{
  KjNode* store = corDbTenantStore(tenantP);

  return kjLookup(store, "subscriptions");
}



// -----------------------------------------------------------------------------
//
// corDbRegistrations - return the "registrations" array for a tenant
//
KjNode* corDbRegistrations(Tenant* tenantP)
{
  KjNode* store = corDbTenantStore(tenantP);

  return kjLookup(store, "registrations");
}

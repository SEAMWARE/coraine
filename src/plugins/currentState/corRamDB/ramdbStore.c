//
// FILE            ramdbStore.c
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

#include "currentState/corRamDB/ramdbStore.h"         // Own interface



// -----------------------------------------------------------------------------
//
// ramdbTenantStore - return (or create) the per-tenant KjNode tree
//
KjNode* ramdbTenantStore(Tenant* tenantP)
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
// ramdbEntities - return the "entities" array for a tenant
//
KjNode* ramdbEntities(Tenant* tenantP)
{
  KjNode* store = ramdbTenantStore(tenantP);

  return kjLookup(store, "entities");
}



// -----------------------------------------------------------------------------
//
// ramdbSubscriptions - return the "subscriptions" array for a tenant
//
KjNode* ramdbSubscriptions(Tenant* tenantP)
{
  KjNode* store = ramdbTenantStore(tenantP);

  return kjLookup(store, "subscriptions");
}



// -----------------------------------------------------------------------------
//
// ramdbRegistrations - return the "registrations" array for a tenant
//
KjNode* ramdbRegistrations(Tenant* tenantP)
{
  KjNode* store = ramdbTenantStore(tenantP);

  return kjLookup(store, "registrations");
}

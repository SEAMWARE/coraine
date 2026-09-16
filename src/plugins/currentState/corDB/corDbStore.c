//
// FILE            corDbStore.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <pthread.h>                                 // pthread_rwlock_init
#include <stddef.h>                                  // NULL
#include <stdlib.h>                                  // malloc

#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjArray, kjChildAdd
#include "kjson/kjLookup.h"                          // kjLookup

#include "db/Tenant.h"                               // Tenant

#include "currentState/corDB/corDbStore.h"         // Own interface



// -----------------------------------------------------------------------------
//
// corDbTenantStore - return (or create) the per-tenant KjNode tree
//
//
// corDbStoreOf - the tenant's store, created on first use
//
CorDbStore* corDbStoreOf(Tenant* tenantP)
{
  if (tenantP->pluginData != NULL)
    return (CorDbStore*) tenantP->pluginData;

  //
  // First access for this tenant - build the store using malloc (NULL
  // allocator): it outlives every request, so it cannot come from a per-request
  // kalloc buffer.
  //
  CorDbStore* storeP = (CorDbStore*) malloc(sizeof(CorDbStore));

  if (storeP == NULL)
    return NULL;

  KjNode* store         = kjObject(NULL, NULL);
  KjNode* entities      = kjArray(NULL, "entities");
  KjNode* subscriptions = kjArray(NULL, "subscriptions");
  KjNode* registrations = kjArray(NULL, "registrations");

  kjChildAdd(store, entities);
  kjChildAdd(store, subscriptions);
  kjChildAdd(store, registrations);

  storeP->tree     = store;
  storeP->idIndex  = NULL;                           // built on the first entity
  storeP->idxSlots = 0;
  storeP->idxCount = 0;
  pthread_rwlock_init(&storeP->lock, NULL);

  tenantP->pluginData = storeP;

  return storeP;
}



// -----------------------------------------------------------------------------
//
// corDbStoreRead / corDbStoreWrite - take the lock, hand back the store
//
// A NULL store (out of memory on first touch) is handed back unlocked, and
// corDbStoreUnlock knows not to unlock it. The alternative - failing to lock and
// carrying on - is the bug this whole file exists to prevent.
//
CorDbStore* corDbStoreRead(Tenant* tenantP)
{
  CorDbStore* storeP = corDbStoreOf(tenantP);

  if (storeP != NULL)
    pthread_rwlock_rdlock(&storeP->lock);

  return storeP;
}



CorDbStore* corDbStoreWrite(Tenant* tenantP)
{
  CorDbStore* storeP = corDbStoreOf(tenantP);

  if (storeP != NULL)
    pthread_rwlock_wrlock(&storeP->lock);

  return storeP;
}



// -----------------------------------------------------------------------------
//
// corDbStoreUnlock - the cleanup handler behind COR_DB_READ / COR_DB_WRITE
//
// Called by the compiler on every exit from the scope that declared the lock,
// including returns the author forgot about. Takes a POINTER to the variable
// because that is the cleanup attribute's contract.
//
void corDbStoreUnlock(CorDbStore** storePP)
{
  if ((storePP != NULL) && (*storePP != NULL))
    pthread_rwlock_unlock(&(*storePP)->lock);
}



// -----------------------------------------------------------------------------
//
KjNode* corDbTenantStore(Tenant* tenantP)
{
  CorDbStore* storeP = corDbStoreOf(tenantP);

  return (storeP != NULL) ? storeP->tree : NULL;
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

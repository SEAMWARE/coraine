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
#include <stdlib.h>                                  // malloc, free

#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeBuilder.h"                  // corTreeObject, corTreeArray, corTreeChildAdd
#include "corTree/corTreeFree.h"                     // corTreeFree
#include "corTree/corTreeLookup.h"                   // corTreeLookup

#include "db/Tenant.h"                               // Tenant

#include "currentState/corDB/corDbStore.h"         // Own interface



// -----------------------------------------------------------------------------
//
// corDbTenantStore - return (or create) the per-tenant CorNode tree
//
//
// corDbStoreOf - the tenant's store, created on first use
//
CorDbStore* corDbStoreOf(Tenant* tenantP)
{
  CorDbStore* existingP = (CorDbStore*) __atomic_load_n(&tenantP->pluginData, __ATOMIC_ACQUIRE);

  if (existingP != NULL)
    return existingP;

  //
  // First access for this tenant - build the store using malloc (NULL
  // allocator): it outlives every request, so it cannot come from a per-request
  // kalloc buffer.
  //
  CorDbStore* storeP = (CorDbStore*) malloc(sizeof(CorDbStore));

  if (storeP == NULL)
    return NULL;

  CorNode* store        = corTreeObject(NULL, NULL);
  CorNode* entities     = corTreeArray(NULL, "entities");
  CorNode* subscriptions = corTreeArray(NULL, "subscriptions");
  CorNode* registrations = corTreeArray(NULL, "registrations");

  corTreeChildAdd(store, entities);
  corTreeChildAdd(store, subscriptions);
  corTreeChildAdd(store, registrations);

  storeP->tree           = store;
  storeP->entities       = entities;
  storeP->idToPrevEntity = NULL;                     // built on the first entity
  storeP->idxSlots = 0;
  storeP->idxCount = 0;
  pthread_rwlock_init(&storeP->lock, NULL);

  //
  // Published with a compare-and-swap. Two requests arriving together for a tenant with no
  // store yet - the first requests after startup, on ANY tenant, the default one included -
  // each built one, and the second assignment replaced the first: the entities already put in
  // it were gone, while their requests had answered 201. Now the first store in wins; a thread
  // that loses the race frees its own and uses the winner's.
  //
  void* expected = NULL;

  if (__atomic_compare_exchange_n(&tenantP->pluginData, &expected, storeP, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) == false)
  {
    pthread_rwlock_destroy(&storeP->lock);
    corTreeFree(storeP->tree);
    free(storeP);
    return (CorDbStore*) expected;
  }

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
CorNode* corDbTenantStore(Tenant* tenantP)
{
  CorDbStore* storeP = corDbStoreOf(tenantP);

  return (storeP != NULL) ? storeP->tree : NULL;
}





// -----------------------------------------------------------------------------
//
// corDbEntities - return the "entities" array for a tenant
//
CorNode* corDbEntities(Tenant* tenantP)
{
  CorNode* store = corDbTenantStore(tenantP);

  return corTreeLookup(store, "entities");
}



// -----------------------------------------------------------------------------
//
// corDbSubscriptions - return the "subscriptions" array for a tenant
//
CorNode* corDbSubscriptions(Tenant* tenantP)
{
  CorNode* store = corDbTenantStore(tenantP);

  return corTreeLookup(store, "subscriptions");
}



// -----------------------------------------------------------------------------
//
// corDbRegistrations - return the "registrations" array for a tenant
//
CorNode* corDbRegistrations(Tenant* tenantP)
{
  CorNode* store = corDbTenantStore(tenantP);

  return corTreeLookup(store, "registrations");
}

#ifndef CORDB_CORDBSTORE_H_
#define CORDB_CORDBSTORE_H_

//
// FILE            corDbStore.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <pthread.h>                                 // pthread_rwlock_t

#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// CorDbStore - one tenant's data, and the lock that guards it
//
// The tree is an object with three array children:
//   { "entities": [...], "subscriptions": [...], "registrations": [...] }
//
// Stored in tenant->pluginData. Created lazily on first access.
//
// THE LOCK IS NOT OPTIONAL. KjNode is built on linked lists and every write
// relinks pointers - in the entity array, and in the child list of the entity
// being changed. Two threads doing that at once do not produce "one of the two
// values", they produce a corrupted list. Measured, before this existed: twenty
// concurrent PATCHes killed the broker with `malloc(): unaligned tcache chunk
// detected`, reproducibly, while ONE connection served 30 433 req/s happily.
//
// A READ-WRITE lock rather than a mutex, because the common case is reads and
// they can all run at once: a query walks the store and clones what it returns,
// which needs the tree to hold still but not to be private. Writers take it
// exclusively, and a write is microseconds - the store is RAM.
//
// Per tenant, because a request is always on exactly one tenant. No request
// needs two of these, so there is no lock-ordering question to get wrong.
//
typedef struct CorDbStore
{
  KjNode*             tree;
  pthread_rwlock_t    lock;

  //
  // id -> KjNode*, so finding one entity is a hash rather than a walk of the
  // whole store doing a kjLookup per entity. Guarded by the same lock as the
  // tree, because it changes exactly when the tree does - see corDbIndex.h.
  //
  struct KHashTable*  idIndex;
  int                 idxSlots;
  int                 idxCount;
} CorDbStore;



// -----------------------------------------------------------------------------
//
// corDbStoreOf - the tenant's store, created on first use
//
extern CorDbStore* corDbStoreOf(Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// corDbStoreRead / corDbStoreWrite / corDbStoreUnlock
//
// Take the lock and hand back the store. The macros below are what callers use;
// these exist because __attribute__((cleanup)) needs a function to call.
//
extern CorDbStore* corDbStoreRead(Tenant* tenantP);
extern CorDbStore* corDbStoreWrite(Tenant* tenantP);
extern void        corDbStoreUnlock(CorDbStore** storePP);



// -----------------------------------------------------------------------------
//
// COR_DB_READ / COR_DB_WRITE - lock for the rest of this scope
//
// One line at the top of a function, and the unlock happens on EVERY exit path,
// including the early returns. That matters more than it looks: these functions
// have two to five returns each and there are two dozen of them, so unlocking
// by hand is some seventy chances to leak a lock - and a leaked write lock is a
// broker that hangs on its next request, which a SEQUENTIAL test suite would
// never notice. The same blind spot that let the corruption in.
//
// GCC/clang cleanup attribute. The whole stack builds with gcc and -Werror.
//
// NOT recursive. A function holding one of these must not call another that
// takes one - corDbEntityMerge calling corDbEntityChangesApply would deadlock,
// which is why the entry points that are ALSO called internally are split into
// a locking shell and an unlocked `...Locked` body.
//
#define COR_DB_READ(tenantP)                                                 \
  CorDbStore* corDbLockedStore __attribute__((cleanup(corDbStoreUnlock))) =  \
    corDbStoreRead(tenantP);                                                 \
  (void) corDbLockedStore

#define COR_DB_WRITE(tenantP)                                                \
  CorDbStore* corDbLockedStore __attribute__((cleanup(corDbStoreUnlock))) =  \
    corDbStoreWrite(tenantP);                                                \
  (void) corDbLockedStore



// -----------------------------------------------------------------------------
//
// corDbTenantStore - the per-tenant KjNode tree
//
extern KjNode* corDbTenantStore(Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// corDbEntities / corDbSubscriptions / corDbRegistrations - the three arrays
//
extern KjNode* corDbEntities(Tenant* tenantP);
extern KjNode* corDbSubscriptions(Tenant* tenantP);
extern KjNode* corDbRegistrations(Tenant* tenantP);

#endif  // CORDB_CORDBSTORE_H_

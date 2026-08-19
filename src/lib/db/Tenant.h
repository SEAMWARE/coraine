#ifndef DB_TENANT_H_
#define DB_TENANT_H_

//
// FILE            Tenant.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                     // bool

#include "kjson/KjNode.h"                                // KjNode



// -----------------------------------------------------------------------------
//
// TENANT_SUB_KIND_* - which of the three subscription caches owns a document
//
// One collection holds all three kinds (entity subs, periodic subs, CSR-subs),
// so every path that caches a subscription has to route it. See
// tenantSubCacheItemStore.
//
#define TENANT_SUB_KIND_NONE    0
#define TENANT_SUB_KIND_ENTITY  1
#define TENANT_SUB_KIND_PERNOT  2
#define TENANT_SUB_KIND_CSR     3



// -----------------------------------------------------------------------------
//
// Tenant - tenant descriptor with pre-built database name
//
typedef struct Tenant
{
  char            name[64];       // tenant name (empty string for default)
  char            dbName[128];    // "prefix" or "prefix-tenantname"
  bool            initialized;    // true after DB setup (indexes created)
  void*           pluginData;     // opaque, owned by the DB plugin
  void*           troePoolP;      // opaque TRoE connection pool, owned by the temporal plugin
  void*           subCacheP;      // entity subscription cache (LdSubCache*), owned by broker
  void*           pernotCacheP;   // periodic notification cache (LdPernotCache*), owned by broker
  void*           regCacheP;      // registration cache (LdRegCache*), owned by broker
  void*           regSubCacheP;   // CSR-subscription cache (LdSubCache*, § 5.11), owned by broker
  void*           entityMapStoreP; // entity map store (LdEntityMapStore*), for distributed query pagination
  void*           snapshotCacheP; // snapshot cache (LdSnapshotCache*, § 5.16), owned by broker
  struct Tenant*  next;           // linked list
} Tenant;



// -----------------------------------------------------------------------------
//
// tenant0 - default tenant (no NGSILD-Tenant header)
//
extern Tenant  tenant0;
extern Tenant* tenantList;



// -----------------------------------------------------------------------------
//
// tenantInit - initialize default tenant with DB prefix
//
extern void    tenantInit(const char* dbPrefix);



// -----------------------------------------------------------------------------
//
// tenantDbPrefixSet - point the default tenant at the configured DB name
//
// Called by the DB plugin's init (which knows the real database name) AFTER
// main has already run tenantInit. Sets the db-name prefix without re-creating
// the caches (which would orphan the ones main allocated).
//
extern void    tenantDbPrefixSet(const char* dbPrefix);



// -----------------------------------------------------------------------------
//
// tenantLookup - find a tenant by name (NULL/empty => default)
//
extern Tenant* tenantLookup(const char* name);



// -----------------------------------------------------------------------------
//
// tenantGetOrCreate - find or allocate a new tenant
//
extern Tenant* tenantGetOrCreate(const char* name);



// -----------------------------------------------------------------------------
//
// tenantFromRequest - resolve tenant from NGSILD-Tenant request header
//
// autoCreate == true:  write operations (POST) — create tenant if new
// autoCreate == false: read operations (GET)  — return NULL + 404 if unknown
//
extern Tenant* tenantFromRequest(bool autoCreate);



// -----------------------------------------------------------------------------
//
// tenantPreServiceHook - corRest preServiceHook for tenant resolution
//
extern bool tenantPreServiceHook(void);



// -----------------------------------------------------------------------------
//
// tenantSubCacheReload - load subscriptions from DB into cache for all tenants
//
// Called once at startup after db.init(). For persistent DB plugins (mongoc),
// this restores the subscription cache from the previous session.
//
extern void tenantSubCacheReload(void);



// -----------------------------------------------------------------------------
//
// tenantRegCacheReload - load registrations from DB into cache for all tenants
//
// Called once at startup after db.init(). For persistent DB plugins (mongoc),
// this restores the registration cache from the previous session.
//
extern void tenantRegCacheReload(void);



// -----------------------------------------------------------------------------
//
// tenantSnapshotCacheReload - load snapshots from DB into cache for all
// tenants. For each persisted snapshot, the metadata is restored to the
// cache and the per-snapshot tenant struct is reconstructed (its DB
// already exists; mongocTenantSetup is idempotent on indexes).
//
// nextSnapSeq on each cache is bumped to max(persisted snapSeq) + 1 so
// new snapshots can't collide with one that's still on disk.
//
extern void tenantSnapshotCacheReload(void);



// -----------------------------------------------------------------------------
//
// tenantSubCacheItemStore - cache one subscription document, in the cache that
// owns it (entity / periodic / CSR-sub). Returns the TENANT_SUB_KIND_* it was
// routed to, TENANT_SUB_KIND_NONE if that cache does not exist.
//
// 'replace' drops a copy already cached under the same id first — needed by any
// caller that may be overwriting (the HA apply), skipped by the startup load,
// where it would make loading N subscriptions O(N²).
//
extern int tenantSubCacheItemStore(Tenant* tP, KjNode* subP, bool replace);



// -----------------------------------------------------------------------------
//
// tenantSubCacheItemDrop - remove a subscription from whichever cache holds it
//
extern bool tenantSubCacheItemDrop(Tenant* tP, const char* subId);



// -----------------------------------------------------------------------------
//
// tenantSubCacheItemRefresh - re-read one subscription from the DB into the cache
//
// Returns false only when the read itself failed. A row that is gone counts as
// success: the cached copy is dropped, which is the correct end state.
//
extern bool tenantSubCacheItemRefresh(Tenant* tP, const char* subId);



// -----------------------------------------------------------------------------
//
// tenantRegCacheItemDrop - remove a registration from the cache
//
extern bool tenantRegCacheItemDrop(Tenant* tP, const char* regId);



// -----------------------------------------------------------------------------
//
// tenantRegCacheItemRefresh - re-read one registration from the DB into the cache
//
extern bool tenantRegCacheItemRefresh(Tenant* tP, const char* regId);

#endif  // DB_TENANT_H_

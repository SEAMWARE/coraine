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

#include "corTree/CorNode.h"                             // CorNode



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
  struct Tenant*  liveTenantP;    // a Snapshot's own tenant: the tenant the snapshot was taken on - NULL for any other
  const char*     snapshotId;     // a Snapshot's own tenant: the snapshot's id (borrowed from its cache item) - NULL for any other
  struct Tenant*  next;           // linked list
  bool            reserved;       // a reserved tenant (tenantReserve): its own database, left out of tenant listings
} Tenant;



// -----------------------------------------------------------------------------
//
// TENANT_RESERVED_DB_MARK - what tells a reserved tenant's database from an ordinary one's: "<prefix>_r-<name>"
// for mongoc (an ordinary tenant's is "<prefix>-<name>"), "_r-<name>" for corDB's directory (an ordinary
// tenant's directory never holds a '_' - corDB escapes it)
//
#define TENANT_RESERVED_DB_MARK  "_r-"
#define TENANT_RESERVED_MAX      8



// -----------------------------------------------------------------------------
//
// tenant0 - default tenant (no NGSILD-Tenant header)
//
extern Tenant  tenant0;
extern Tenant* tenantList;



// -----------------------------------------------------------------------------
//
// tenantApiName - the name a client knows the tenant by (NGSILD-Tenant; "" = the default tenant)
//
// A Snapshot's own tenant is known by the tenant the snapshot was taken on - its internal name
// ("<tenant>-_snap_<seq>") is a DB name, never on the wire.
//
extern const char* tenantApiName(Tenant* tP);



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
extern int tenantSubCacheItemStore(Tenant* tP, CorNode* subP, bool replace);



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




// -----------------------------------------------------------------------------
//
// tenantReserve - make a tenant name a reserved tenant's (--reservedTenants; a plugin: cep.so its rules tenant)
//
// A reserved tenant is reached as any other (NGSILD-Tenant: <name>, the broker's access control), but its data
// lives apart - a database of its own, under TENANT_RESERVED_DB_MARK - and it is left out of /admin/tenants and
// of what plugins see as the tenants (tenantIsData). corCep keeps its rules in one: an ordinary tenant's
// queries, /types and /attributes never meet a rule.
//
// Before the tenant exists, or when it already is reserved. False when an ORDINARY tenant of that name exists
// (its data is in an ordinary database - reserving the name would not move it), when the name is empty or too
// long, or when TENANT_RESERVED_MAX names are reserved already. Never fatal: the caller goes on with the name an
// ordinary tenant's (cep.so: its rules in the ordinary tenant).
//
extern bool tenantReserve(const char* name);



// -----------------------------------------------------------------------------
//
// tenantUnreserve - the name an ordinary tenant's again: the DB plugin found ordinary data under a name reserved
// (--reservedTenants, or a reserved database found first) - ordinary data wins, it is never hidden. At start only,
// before any request (the reserved names are read lock-free).
//
extern void tenantUnreserve(const char* name);



// -----------------------------------------------------------------------------
//
// tenantIsReserved - is the name a reserved tenant's (case-insensitive, as tenant names are)
//
extern bool tenantIsReserved(const char* name);



// -----------------------------------------------------------------------------
//
// tenantIsData - a tenant that holds data: not reserved, not a Snapshot's own
//
extern bool tenantIsData(Tenant* tP);



// -----------------------------------------------------------------------------
//
// tenantDataVisit - fn for the default tenant ("") and every tenant that holds data (tenantIsData), by the name a
// client knows it by - what a plugin's "every tenant" is (cep.so: a rule with no 'tenant' runs in each). Lock-free:
// the list is only ever prepended to, and a tenant is published complete; one made meanwhile may be left out.
//
extern void tenantDataVisit(void (*fn)(const char* name, void* ctx), void* ctx);

#endif  // DB_TENANT_H_

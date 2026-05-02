#ifndef DB_SNAPSHOTTENANT_H_
#define DB_SNAPSHOTTENANT_H_

//
// FILE            snapshotTenant.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Per-snapshot DB tenant — a Tenant struct dedicated to holding the
// frozen entity bodies of a single snapshot. The struct is NOT added
// to the global tenantList (tenant lookup by name skips it); it is
// owned by the LdSnapshotCacheItem that points at it.
//
// Naming: <originalTenantName>__snap__<snapSeq:hex>. The snapSeq is
// the monotonic per-tenant sequence assigned by ldSnapshotCacheItemAdd
// — collision-free across the lifetime of the broker (and across
// restart, once Phase #140 lands persistence + reload).
//
#include "db/Tenant.h"                                   // Tenant


// -----------------------------------------------------------------------------
//
// snapshotTenantCreate - allocate and initialise a snap-tenant.
//
// origP    : the originating tenant (may be &tenant0 for the default).
// snapSeq  : monotonic sequence number from the cache item.
//
// On success the returned Tenant has dbName ready and tenantSetup has
// been invoked on the active DB plugin (so the storage is provisioned).
// Subscriptions / registrations / entityMap / snapshot caches are NOT
// created — a snap-tenant is read-only by design and never participates
// in subscriptions or distops.
//
// Returns NULL if the tenant name would overflow tenant.dbName, or if
// db.tenantSetup fails.
//
extern Tenant* snapshotTenantCreate(Tenant* origP, int snapSeq);



// -----------------------------------------------------------------------------
//
// snapshotTenantDestroy - free the snap-tenant struct.
//
// NOTE: this does NOT drop the underlying DB collections. Phase #140
// will add db.tenantDrop and call it from this path; until then the
// data is left orphaned on disk after a snapshot is deleted.
//
extern void snapshotTenantDestroy(Tenant* snapTenantP);

#endif  // DB_SNAPSHOTTENANT_H_

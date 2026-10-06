#ifndef DB_SNAPSHOTTENANT_H_
#define DB_SNAPSHOTTENANT_H_

//
// FILE            snapshotTenant.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Per-snapshot DB tenant — a Tenant struct dedicated to holding the
// entities of a single snapshot (and, with COR_FEATURE_SNAPSHOT_SUBSCRIPTIONS,
// its subscriptions). The struct is NOT added to the global tenantList
// (tenant lookup by name skips it); it is owned by the LdSnapshotCacheItem
// that points at it.
//
// Naming: <originalTenantName>-_snap_<snapSeq:hex>. The snapSeq is
// the monotonic per-tenant sequence assigned by ldSnapshotCacheItemAdd,
// persisted with the snapshot (_snapSeq) - collision-free across the
// lifetime of the broker and across a restart.
//
#include "db/Tenant.h"                                   // Tenant


// -----------------------------------------------------------------------------
//
// snapshotTenantCreate - allocate and initialise a snap-tenant.
//
// origP    : the originating tenant (may be &tenant0 for the default).
// itemP    : the snapshot's cache item - its snapSeq names the tenant, its id is the tenant's
//            snapshotId (borrowed: the item outlives its tenant, snapshotItemDestroy).
//
// On success the returned Tenant has dbName ready and tenantSetup has
// been invoked on the active DB plugin (so the storage is provisioned).
// With COR_FEATURE_SNAPSHOT_SUBSCRIPTIONS it has the subscription caches (entity + periodic) -
// subscriptions on the snapshot, § 7.9.2. No registration, CSR-subscription, entityMap or snapshot
// caches: a snapshot is local scope and has no snapshots of its own.
//
// Returns NULL if the tenant name would overflow tenant.dbName, or if
// db.tenantSetup fails.
//
struct LdSnapshotCacheItem;
extern Tenant* snapshotTenantCreate(Tenant* origP, struct LdSnapshotCacheItem* itemP);



// -----------------------------------------------------------------------------
//
// snapshotTenantDestroy - free the snap-tenant struct and its caches
//
// The DB and TRoE stores are dropped before, by snapshotItemDestroy.
//
extern void snapshotTenantDestroy(Tenant* snapTenantP);



#if COR_FEATURE_SNAPSHOT_SUBSCRIPTIONS
// -----------------------------------------------------------------------------
//
// snapshotTenantsVisit - call 'visit' for each Snapshot's own tenant of 'liveP'
//
// For the loops that serve subscriptions on a Snapshot (periodic, throttle flush, stats flush). The
// snapshot is PINNED across its visit - a DELETE of it meanwhile destroys the tenant and its caches
// at the unpin, not under the visit - and no cache lock is held across it.
//
typedef void (*SnapshotTenantVisitFn)(Tenant* snapTenantP, void* arg);
extern void snapshotTenantsVisit(Tenant* liveP, SnapshotTenantVisitFn visit, void* arg);
#endif



// -----------------------------------------------------------------------------
//
// snapshotItemDestroy - the snapshot cache's destroy hook (see snapshotTenant.c)
//
extern void snapshotItemDestroy(struct LdSnapshotCacheItem* itemP);



#endif  // DB_SNAPSHOTTENANT_H_

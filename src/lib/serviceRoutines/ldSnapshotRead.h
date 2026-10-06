#ifndef SERVICEROUTINES_LDSNAPSHOTREAD_H_
#define SERVICEROUTINES_LDSNAPSHOTREAD_H_

//
// FILE            ldSnapshotRead.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Snapshot-aware read paths (§ 5.16, Phase 2b).
//
// When the NGSILD-Snapshot HTTP header is present (§ 6.3.22), a GET
// /entities/{id} or GET /entities is served from the named snapshot's
// frozen entity store rather than the live database. The operations
// are implicitly local-only (§ 5.5.15) — no distop forwarding, no
// reg-cache consultation.
//
#include <stdbool.h>                                     // bool
#include "corNgsild/LdSnapshotCache.h"                    // LdSnapshotCacheItem


// -----------------------------------------------------------------------------
//
// ldSnapshotItemFromHeader -
//
// Read NGSILD-Snapshot from the inbound request and look it up on the
// current tenant's snapshot cache. Side effects:
//   - sets *seenP to true if the header was supplied (regardless of
//     whether the lookup succeeded);
//   - on success, echoes "NGSILD-Snapshot: <id>" back per § 6.3.22 and
//     returns the cache item;
//   - on failure (cache missing OR id unknown) raises a 404
//     ResourceNotFound and returns NULL.
//
// Caller flow: first probe with seenP — if false, the request isn't
// snapshot-targeted and the caller proceeds with the live path. If
// true and the return value is NULL, the helper has already raised an
// error and the caller must just return.
//
extern LdSnapshotCacheItem* ldSnapshotItemFromHeader(bool* seenP);



// -----------------------------------------------------------------------------
//
// snapshotGetEntity - serve GET /entities/{id} from a snapshot.
//
// Looks up entityId in itemP->entities, deep-clones the match into the
// request arena (so the response renderer can mutate freely), and
// publishes it as corRest.out.responseTree. Returns true; raises 404 if
// the entity isn't part of the snapshot.
//
extern bool snapshotGetEntity(LdSnapshotCacheItem* itemP, const char* entityId);



// -----------------------------------------------------------------------------
//
// snapshotGetEntities - serve GET /entities from a snapshot.
//
// Walks itemP->entities, applies the standard local filters
// (idV/idPattern, typeV/typeExpr, qExpr, scopeExpr, geoQ via the same
// db.geoMatchFunc the live path uses), pagination, orderBy and
// pick/omit. No distop forwarding — § 5.5.15 forces local scope.
//
extern bool snapshotGetEntities(LdSnapshotCacheItem* itemP);



// -----------------------------------------------------------------------------
//
// ldSnapshotWriteGuard - dispatcher-level guard for a non-GET request carrying NGSILD-Snapshot.
//
// COR_FEATURE_SNAPSHOT_WRITE: an operation on Entities or their Temporal Evolution (§ 7.9.2) is
// routed to the snapshot's own tenant, in a local scope - corNgsild.tenantP becomes the snapshot's,
// corNgsild.local true. Any other operation (subscriptions, registrations, ...) is a 422
// OperationNotSupported. Without the feature every non-GET is a 422 (snapshots read-only).
// Reads pass through - each read routine routes itself, and always locally: the snap-tenant
// holds the federated view captured at create time, and a distop fan-out would mix live data
// into it.
//
// Returns true if the request may proceed, false if the guard fired
// and set a problem detail (caller should abort).
//
extern bool ldSnapshotWriteGuard(void);

#endif  // SERVICEROUTINES_LDSNAPSHOTREAD_H_

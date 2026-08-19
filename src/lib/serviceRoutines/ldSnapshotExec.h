#ifndef SERVICEROUTINES_LDSNAPSHOTEXEC_H_
#define SERVICEROUTINES_LDSNAPSHOTEXEC_H_

//
// FILE            ldSnapshotExec.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Snapshot query execution (§ 5.16, Phase 2a).
//
// Executes the per-Query (§ 5.2.23) entries inside `snapshotQueries`,
// deep-clones the matched entities into the snapshot's frozen store
// and builds the `snapshotQueriesDetails` array (§ 5.2.42). Updates
// `snapshotStatus` on the snapshot tree per § 5.16.1.4.
//
// Phase 2a runs queries synchronously on POST. Phase 3 will move
// execution to a background worker so the create call can return
// before queries finish.
//
#include <stdbool.h>                                     // bool

#include "kjson/KjNode.h"                                // KjNode
#include "corNgsild/LdSnapshotCache.h"                    // LdSnapshotCache, LdSnapshotCacheItem
#include "db/Tenant.h"                                   // Tenant


// -----------------------------------------------------------------------------
//
// ldSnapshotExecQueries -
//
// Walks `snapshotQueries` on itemP->tree, runs each Query against the
// local DB and matching CSRs, streams results into the snapshot's own
// tenant via db.entityCreate, builds snapshotQueriesDetails on
// itemP->tree, and updates itemP->status (and the snapshotStatus
// field on the cached tree) to the aggregate outcome.
//
// Thread-safe: writes to itemP->tree use NULL allocator (malloc-backed
// persistent memory), so the function is safe to call from the request
// thread OR a background worker. db calls use the per-thread corRest
// arena for transient buffers.
//
// Returns true on completion (failures are recorded as per-query
// "failure" details, not propagated as a routine-level error).
//
extern bool ldSnapshotExecQueries(LdSnapshotCache*     cacheP,
                                  LdSnapshotCacheItem* itemP,
                                  Tenant*              tenantP);

#endif  // SERVICEROUTINES_LDSNAPSHOTEXEC_H_

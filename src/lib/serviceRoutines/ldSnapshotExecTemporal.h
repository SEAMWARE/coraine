#ifndef SERVICEROUTINES_LDSNAPSHOTEXECTEMPORAL_H_
#define SERVICEROUTINES_LDSNAPSHOTEXECTEMPORAL_H_

//
// FILE            ldSnapshotExecTemporal.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Snapshot temporal-query execution (§ 5.16.1.4 / Phase #146b).
//
// Walks `snapshotTemporalQueries` on itemP->tree, runs each Query
// against the live tenant's TRoE store via troe.entityTemporalQuery,
// streams matched EntityTemporal trees into the snap-tenant's TRoE
// store via troe.entityTemporalCreate, and builds
// `snapshotTemporalQueriesDetails` (§ 5.2.42) on itemP->tree.
//
// After running, re-computes snapshotStatus by aggregating the
// per-query result-status across BOTH snapshotQueriesDetails and
// snapshotTemporalQueriesDetails — so the broker reports the
// combined outcome of current-state + temporal capture.
//
// No-op (returns true) when snapshotTemporalQueries is absent or
// empty, when troe is uninitialised, or when the active TRoE plugin
// has no entityTemporalQuery / entityTemporalCreate.
//
// Local-only first cut: distop forwarding deferred (matches the
// live GET /temporal/entities path; multi-source temporal
// federation lands in a later slice).
//
#include <stdbool.h>                                     // bool

#include "swNgsild/LdSnapshotCache.h"                    // LdSnapshotCache, LdSnapshotCacheItem
#include "db/Tenant.h"                                   // Tenant


extern bool ldSnapshotExecTemporalQueries(LdSnapshotCache*     cacheP,
                                          LdSnapshotCacheItem* itemP,
                                          Tenant*              tenantP);

#endif  // SERVICEROUTINES_LDSNAPSHOTEXECTEMPORAL_H_

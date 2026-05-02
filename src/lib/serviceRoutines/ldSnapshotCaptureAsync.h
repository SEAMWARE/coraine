#ifndef SERVICEROUTINES_LDSNAPSHOTCAPTUREASYNC_H_
#define SERVICEROUTINES_LDSNAPSHOTCAPTUREASYNC_H_

//
// FILE            ldSnapshotCaptureAsync.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Background-thread Snapshot capture (§ 5.16.1.4 / § 5.16.2.4 — "the
// following is executed in the background"). Spawned by postSnapshot /
// cloneSnapshot when --asyncSnapshot is set; runs ldSnapshotExecQueries,
// persists the final status via db.snapshotUpdate, and fires
// SnapshotNotification.
//
// The thread is detached. Caller returns 201 immediately with
// snapshotStatus=preparing; the client polls via GET /snapshots/{id}
// or waits for the SnapshotNotification.
//
#include <stdbool.h>                                     // bool
#include "swNgsild/LdSnapshotCache.h"                    // LdSnapshotCache, LdSnapshotCacheItem
#include "db/Tenant.h"                                   // Tenant


// -----------------------------------------------------------------------------
//
// ldSnapshotCaptureAsync - spawn a detached worker thread to run the
// capture for `itemP`. Captures the splitEntities setting at spawn
// time (so the per-request URL param survives into the worker).
//
// itemP MUST already be in the cache and have snapTenantP set; the
// worker mutates itemP->tree and itemP->status as capture proceeds.
//
extern void ldSnapshotCaptureAsync(LdSnapshotCache*     cacheP,
                                    LdSnapshotCacheItem* itemP,
                                    Tenant*              tenantP,
                                    bool                 splitEntitiesSet,
                                    bool                 splitEntitiesVal);

#endif  // SERVICEROUTINES_LDSNAPSHOTCAPTUREASYNC_H_

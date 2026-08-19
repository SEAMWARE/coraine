#ifndef ADMIN_SUB_STATS_H
#define ADMIN_SUB_STATS_H

//
// FILE            adminSubStats.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                // bool



// -----------------------------------------------------------------------------
//
// adminPostSubStatsFlush - POST /admin/subStats/flush
//
// Walks the entity-sub + CSR-sub caches of every tenant and flushes
// pending delta counters to the current-state DB plugin. Synchronous —
// the caller's request blocks until every reachable sub has been
// flushed (or the plugin errors).
//
// Response: 204 No Content on success.
//
extern bool adminPostSubStatsFlush(void);

#endif  // ADMIN_SUB_STATS_H

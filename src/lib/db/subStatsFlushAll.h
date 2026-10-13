#ifndef COR_BROKER_SUB_STATS_FLUSH_ALL_H
#define COR_BROKER_SUB_STATS_FLUSH_ALL_H

//
// FILE            subStatsFlushAll.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Walk every tenant + every sub cache (entity / CSR / pernot) and
// flush delta counters to the current-state DB plugin. Shared by:
//
//  - /admin/subStats/flush  (on-demand admin endpoint)
//  - ldStatsFlushLoop       (periodic background timer)
//



// -----------------------------------------------------------------------------
//
// subStatsFlushAll -
//
// Synchronous. Safe to call when no tenants exist — it's a no-op.
// Safe to call when the current-state DB plugin doesn't support
// stats-flush (NULL callback → ldSubStatsFlush is a no-op).
//
extern void subStatsFlushAll(void);

#endif  // COR_BROKER_SUB_STATS_FLUSH_ALL_H

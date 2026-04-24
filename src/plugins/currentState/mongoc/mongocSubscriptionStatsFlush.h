#ifndef MONGOC_SUBSCRIPTION_STATS_FLUSH_H
#define MONGOC_SUBSCRIPTION_STATS_FLUSH_H

//
// FILE            mongocSubscriptionStatsFlush.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stdint.h>                                  // uint64_t

#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// mongocSubscriptionStatsFlush -
//
// Persist a delta of notification counters for one subscription to mongo,
// using $inc for the counter deltas (HA-safe under concurrent flushes from
// other broker instances) and $set for the last-* timestamps.
//
extern int mongocSubscriptionStatsFlush(Tenant*      tenantP,
                                        const char*  subId,
                                        int          deltaSent,
                                        int          deltaFailed,
                                        uint64_t     lastNotification,
                                        uint64_t     lastSuccess,
                                        uint64_t     lastFailure);

#endif  // MONGOC_SUBSCRIPTION_STATS_FLUSH_H

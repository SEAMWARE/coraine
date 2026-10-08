#ifndef MIGRATE_MIGRATEHISTORY_H_
#define MIGRATE_MIGRATEHISTORY_H_

//
// FILE            migrateHistory.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                      // bool

#include "corTree/CorNode.h"                              // CorNode

#include "db/Tenant.h"                                    // Tenant
#include "migrate/MigrateState.h"                         // MigrateState



// -----------------------------------------------------------------------------
//
// migrateTemporalEntity - queue one entity-level history event (created / replaced / deleted)
//
extern bool migrateTemporalEntity(MigrateState* msP, Tenant* tenantP, CorNode* dataP);



// -----------------------------------------------------------------------------
//
// migrateTemporalInstance - queue one attribute instance of the history
//
extern bool migrateTemporalInstance(MigrateState* msP, Tenant* tenantP, CorNode* dataP);



// -----------------------------------------------------------------------------
//
// migrateHistoryBatchFull - time to flush?
//
extern bool migrateHistoryBatchFull(MigrateState* msP);



// -----------------------------------------------------------------------------
//
// migrateHistoryFlush - hand the queued events to the TRoE plugin, as one batch
//
extern bool migrateHistoryFlush(MigrateState* msP);

#endif  // MIGRATE_MIGRATEHISTORY_H_

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



// -----------------------------------------------------------------------------
//
// migrateHistorySeen - the stream holds history for this entity
//
extern void migrateHistorySeen(MigrateState* msP, Tenant* tenantP, const char* entityId);



// -----------------------------------------------------------------------------
//
// migrateEntityImported - an entity imported as current state (remembered only with a TRoE store)
//
extern void migrateEntityImported(MigrateState* msP, Tenant* tenantP, const char* entityId);



// -----------------------------------------------------------------------------
//
// migrateHistoryCreatedRows - at the end of the stream, with a TRoE store: an imported entity the
// stream gave no history gets the history a create writes - its "created" event at its createdAt and
// an instance of each attribute as it is, at its own times. An entity with history in the stream gets
// what the stream holds, nothing more.
//
extern void migrateHistoryCreatedRows(MigrateState* msP);

#endif  // MIGRATE_MIGRATEHISTORY_H_

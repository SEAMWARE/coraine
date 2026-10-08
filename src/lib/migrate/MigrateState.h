#ifndef MIGRATE_MIGRATESTATE_H_
#define MIGRATE_MIGRATESTATE_H_

//
// FILE            MigrateState.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The import of a migration stream (coraine-import, doc/migration.md): one record per line, each
// written through the broker's own code into the stores the importer was started with.
//
#include <stdint.h>                                       // uint64_t

#include "corAlloc/CorAlloc.h"                            // CorAlloc
#include "corHash/corHash.h"                              // CorHashTable

#include "db/Tenant.h"                                    // Tenant
#include "troe/TroeDriver.h"                              // TroeEvent



// -----------------------------------------------------------------------------
//
// MigrateKind - the kinds of record a migration stream carries
//
typedef enum MigrateKind
{
  MigrateEntity = 0,
  MigrateSubscription,
  MigrateRegistration,
  MigrateTemporalEntity,
  MigrateTemporalInstance,
  MigrateKinds
} MigrateKind;



// -----------------------------------------------------------------------------
//
// MigrateImported - an entity imported as current state, for its "created" history row at the end
//
typedef struct MigrateImported
{
  Tenant*                  tenantP;
  const char*              entityId;                  // in MigrateState.idAlloc
  struct MigrateImported*  next;
} MigrateImported;



// -----------------------------------------------------------------------------
//
// MigrateState - one import run
//
typedef struct MigrateState
{
  const char*    path;                  // the stream, for the error lines
  int            lineNo;                // the record being imported

  int            okV[MigrateKinds];     // imported, per kind
  int            failedV[MigrateKinds]; // refused, per kind

  //
  // History is written in batches - one transaction for timescale. The events live in the import
  // arena, which is therefore not reset while any are pending.
  //
  TroeEvent*     pendingHead;
  TroeEvent*     pendingTail;
  int            pendingN;
  int            pendingFirstLine;      // the line of the first pending event, for the error report
  int            pendingKindN[MigrateKinds];
  Tenant*        pendingTenantP;

  //
  // With a TRoE store, an entity the stream gives no history gets the row a create writes, at its
  // createdAt: which entities were imported, and which have history in the stream (keyed
  // "<tenant>\n<entity id>"). Both live in idAlloc - malloc, for the whole run.
  //
  CorAlloc          idAlloc;
  char*             idBuffer;
  CorHashTable*     historyIdsP;
  MigrateImported*  importedHead;
  MigrateImported*  importedTail;
  int               createdRowN;            // entities given their created row, for the report
  int               createdRowFailedN;
  int               pendingCreatedN;        // ... of the pending events: entities' created rows
} MigrateState;



// -----------------------------------------------------------------------------
//
// migrateFail - report a refused record (stderr - the import's report; the log is stdout) and count it
//
// Returns false, so a caller can 'return migrateFail(...)'.
//
extern bool migrateFail(MigrateState* msP, MigrateKind kind, const char* format, ...) __attribute__((format(printf, 3, 4)));

#endif  // MIGRATE_MIGRATESTATE_H_

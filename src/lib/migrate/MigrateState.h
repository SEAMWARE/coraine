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
// The import of a migration stream (--importFile, doc/migration.md): one record per line, each
// written through the broker's own code into the stores the broker was started with.
//
#include <stdint.h>                                       // uint64_t

#include "corJsonld/CorLdContext.h"                       // CorLdContext

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
// MigrateState - one import run
//
typedef struct MigrateState
{
  CorLdContext*  contextP;              // --importContext, NULL: the names must be IRIs or core terms
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
} MigrateState;



// -----------------------------------------------------------------------------
//
// migrateFail - report a refused record (stderr - the import's report; the log is stdout) and count it
//
// Returns false, so a caller can 'return migrateFail(...)'.
//
extern bool migrateFail(MigrateState* msP, MigrateKind kind, const char* format, ...) __attribute__((format(printf, 3, 4)));

#endif  // MIGRATE_MIGRATESTATE_H_

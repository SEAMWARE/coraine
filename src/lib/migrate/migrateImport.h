#ifndef MIGRATE_MIGRATEIMPORT_H_
#define MIGRATE_MIGRATEIMPORT_H_

//
// FILE            migrateImport.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//



// -----------------------------------------------------------------------------
//
// migrateImport - import a migration stream (doc/migration.md) into the broker's stores
//
// path:        the stream, one record per line ("-": stdin)
// contextRef:  the @context that expands the names of a stream not already expanded - a URL, or a
//              file holding {"@context": ...}. NULL: every name must be an IRI or a core term.
//
// Runs after the DB and TRoE plugins are up and before the broker serves anything; the broker
// exits after it. Returns the number of records refused (each reported on stderr, path:line; the log is stdout),
// -1 when the import could not start.
//
extern int migrateImport(const char* path, const char* contextRef);

#endif  // MIGRATE_MIGRATEIMPORT_H_

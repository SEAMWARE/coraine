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
// migrateImport - import a migration stream (doc/migration.md) into the stores
//
// path:  the stream, one record per line ("-": stdin), expanded NGSI-LD
//
// Run by coraine-import once the DB and TRoE plugins are up. Returns the number of records refused
// (each reported on stderr, path:line; the log is stdout), -1 when the import could not start.
//
extern int migrateImport(const char* path);

#endif  // MIGRATE_MIGRATEIMPORT_H_

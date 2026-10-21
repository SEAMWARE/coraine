#ifndef TIMESCALE_TIMESCALEMIGRATE_H_
#define TIMESCALE_TIMESCALEMIGRATE_H_

//
// FILE            timescaleMigrate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Apply pending schema migrations against the supplied connection's database.
// Idempotent: each migration is gated on a row in troe_schema_version,
// the meta-table itself is created CREATE TABLE IF NOT EXISTS. Run once per
// tenant database when its pool is first created.
//
// Returns 0 on success, -1 on any failure.
//

#include <libpq-fe.h>                                     // PGconn

extern int timescaleMigrate(PGconn* conn);



// -----------------------------------------------------------------------------
//
// timescaleIndexesParse - the indexes of --troeIndexes (a comma list, "none" or empty: none), for
// timescaleMigrate to create - and to drop the others. -1 for a name it does not know.
//
extern int timescaleIndexesParse(const char* list);

#endif  // TIMESCALE_TIMESCALEMIGRATE_H_

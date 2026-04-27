#ifndef TIMESCALE_TIMESCALEMIGRATE_H_
#define TIMESCALE_TIMESCALEMIGRATE_H_

//
// FILE            timescaleMigrate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Apply pending schema migrations against the connected database.
// Idempotent: each migration is gated on a row in troe_schema_version,
// the meta-table itself is created CREATE TABLE IF NOT EXISTS.
//
// Returns 0 on success, -1 on any failure.
//

extern int timescaleMigrate(void);

#endif  // TIMESCALE_TIMESCALEMIGRATE_H_

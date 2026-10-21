#ifndef TIMESCALE_TIMESCALEWRITER_H_
#define TIMESCALE_TIMESCALEWRITER_H_

//
// FILE            timescaleWriter.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The TRoE writers of a tenant (--troeWriters): threads that send the units (timescaleUnit.h) of
// MANY requests in one transaction.
//
// A transaction per request made Postgres commit, plan and route rows to their chunk once per request;
// under load the cost per transaction, not the rows, was what Postgres spent its cores on. A request
// now renders its rows and queues them; a writer takes every unit queued (up to a cap) and sends them
// as one INSERT per table - while it is busy, the next batch builds up. Each writer has a connection of
// its own, outside the pool (--troePoolSize is the connections of the reads and the history writes).
//
// A unit submitted with wait (--troeSync) returns once its transaction committed - a group commit. A
// read, and a history write of the Temporal API, wait for every unit queued before them
// (timescaleWriterBarrier, called by timescaleConnGet), so they see what was recorded before them.
//

#include <stdbool.h>                                      // bool

#include "db/Tenant.h"                                    // Tenant

#include "temporal/timescale/timescaleUnit.h"             // TsUnit

struct TimescalePool;



// -----------------------------------------------------------------------------
//
// timescaleWriterStart - start the writers of a pool (none for --troeWriters 0)
//
extern int timescaleWriterStart(struct TimescalePool* poolP);



// -----------------------------------------------------------------------------
//
// timescaleWriterStop - send what is queued, then stop the writers of a pool
//
extern void timescaleWriterStop(struct TimescalePool* poolP);



// -----------------------------------------------------------------------------
//
// timescaleWriterSubmit - hand a unit over to the tenant's writers (they free it)
//
// Returns false when the tenant has no writers (--troeWriters 0) - the unit is still the caller's.
// Otherwise true, and with wait, *rcP is the outcome of the unit's transaction.
//
extern bool timescaleWriterSubmit(Tenant* tenantP, TsUnit* uP, bool wait, int* rcP);



// -----------------------------------------------------------------------------
//
// timescaleWriterBarrier - wait until every unit queued for the pool so far is in the database
//
extern void timescaleWriterBarrier(struct TimescalePool* poolP);

#endif  // TIMESCALE_TIMESCALEWRITER_H_

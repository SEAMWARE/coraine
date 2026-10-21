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
// The database down - the connection gone, or the server unable to take writes (timescaleUnitsExec's
// downP): the units stay queued, and one writer tries again, 100 ms later, then twice as late each
// time, up to 5 s. The current state is served as before: a --troeSync request stops waiting for its
// commit, a barrier stops waiting for the queue, an import is told its rows did not go in. Past the
// queue's cap (8 MiB) a request's rows are LOST - counted and logged (timescaleHistoryLost), never
// waited for. With --troeDownExit the broker exits instead: once the database has been unreachable
// that long, or at the first row it would lose; a --troeSync request then waits for its commit, so
// no response leaves without its history.
//

#include <stdbool.h>                                      // bool

#include "db/Tenant.h"                                    // Tenant
#include "troe/TroeDriver.h"                              // TroeHistoryStatus

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
// TsWait - what a submitter waits for
//
typedef enum TsWait
{
  TsWaitNone,      // nothing - the deferred dispatch
  TsWaitCommit,    // the commit, while the database can be reached (--troeSync)
  TsWaitResult     // the outcome - the caller reports it (an import): never queued while the database is down
} TsWait;



// -----------------------------------------------------------------------------
//
// timescaleWriterSubmit - hand a unit over to the tenant's writers (they free it)
//
// Returns false when the tenant has no writers (--troeWriters 0) - the unit is still the caller's.
// Otherwise true, and *rcP is TROE_OK once the unit's transaction committed - for TsWaitNone, once
// the unit is queued; TROE_ERR when it is not recorded, or not yet.
//
extern bool timescaleWriterSubmit(Tenant* tenantP, TsUnit* uP, TsWait wait, int* rcP);



// -----------------------------------------------------------------------------
//
// timescaleWriterBarrier - wait until every unit queued for the pool so far is in the database
//
extern void timescaleWriterBarrier(struct TimescalePool* poolP);



// -----------------------------------------------------------------------------
//
// timescaleHistoryLost - rowN rows of history are lost for a reason of the database's (why): counted,
// logged at most every 10 s - or, with --troeDownExit, the broker exits
//
extern void timescaleHistoryLost(const char* dbName, int rowN, const char* why);



// -----------------------------------------------------------------------------
//
// timescaleHistoryStatus - TroeDriver.historyStatus: over every tenant's writers
//
extern void timescaleHistoryStatus(TroeHistoryStatus* sP);

#endif  // TIMESCALE_TIMESCALEWRITER_H_

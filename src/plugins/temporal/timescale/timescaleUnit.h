#ifndef TIMESCALE_TIMESCALEUNIT_H_
#define TIMESCALE_TIMESCALEUNIT_H_

//
// FILE            timescaleUnit.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// TsUnit - the TRoE rows of one event list (one request's events), rendered as the elements of SQL
// array literals, an array per column. Units are self-contained (malloc'd, nothing of the request's arena), so a
// writer thread can send them after the request is gone, and several of them in one statement: the
// rows of every unit of a batch go in ONE INSERT per table (timescaleUnitsExec), one array per column.
//

#include <stdbool.h>                                      // bool
#include <stdint.h>                                       // uint64_t
#include <pthread.h>                                      // pthread_cond_t
#include <libpq-fe.h>                                     // PGconn



// -----------------------------------------------------------------------------
//
// TsBuf - a growing, malloc'd text buffer
//
typedef struct TsBuf
{
  char*  s;
  int    len;
  int    size;
} TsBuf;



// -----------------------------------------------------------------------------
//
// TsUnit -
//
// A row is an element in each of its table's columns: the rows go to the database as one array per
// column, unnested - INSERT ... SELECT ... FROM unnest('{...}'::text[], ...). As a VALUES list of 1000
// rows, Postgres spent more time parsing and planning the statement's 16 000 expressions than
// inserting its rows (55 ms for 1000 rows of troe_attrs, of which 25 executing; unnested: 22).
//
typedef struct TsUnit
{
  struct TsUnit*  next;          // writer queue linkage
  uint64_t        seq;           // writer queue position (timescaleWriterBarrier)

  TsBuf           entityCol[4];  // troe_entities: entity_id, entity_type, modified_at (s), op
  int             entityRowN;
  TsBuf           attrCol[16];   // troe_attrs: the columns of timescaleUnitsExec's unnest, in its order
  int             attrRowN;
  bool            oom;           // a row did not fit in memory - the unit is not sent

  bool            wait;          // a submitter waits for the unit - under the writer's mutex, false once it is gone
  bool            waitResult;    // ... for its outcome (an import): not queued while the database is down
  bool            done;          // set by the writer, under its mutex, for a waited-for unit
  bool            released;      // the database went down: the submitter stops waiting, the unit stays queued
  pthread_cond_t  doneCond;      // signalled with done - initialised only for a waited-for unit
  int             rc;            // TROE_OK / TROE_ERR, valid once done
} TsUnit;



// -----------------------------------------------------------------------------
//
// tsBufText - printf-append
//
extern bool tsBufText(TsBuf* bP, const char* fmt, ...) __attribute__((format(printf, 2, 3)));



// -----------------------------------------------------------------------------
//
// tsBufElement - append s as an element of a column: an array element ("..." - NULL is NULL), inside
// the SQL literal of the array, so the quotes are doubled as well
//
// Only quotes are doubled for SQL: every connection runs with standard_conforming_strings on
// (timescaleConnect), where a backslash is an ordinary character - and so the array's own escapes
// (\" and \\) reach the array parser as they were written.
//
extern bool tsBufElement(TsBuf* bP, bool first, const char* s);



// -----------------------------------------------------------------------------
//
// tsUnitNew / tsUnitFree -
//
extern TsUnit* tsUnitNew(void);
extern void    tsUnitFree(TsUnit* uP);



// -----------------------------------------------------------------------------
//
// tsUnitBytes - the size of the rows of a unit (the writer's batch and queue caps)
//
extern int tsUnitBytes(const TsUnit* uP);



// -----------------------------------------------------------------------------
//
// timescaleUnitsExec - the rows of unitN units as one INSERT into troe_entities and one into troe_attrs,
// in ONE round trip, atomically (a multi-statement simple query is one implicit transaction - or a part
// of the caller's own, inside a BEGIN)
//
// troe_entities first: troe_attrs' type fallback reads it.
//
// *downP, on a failure: the database's state made it fail, not the rows - the connection is gone, or
// the server cannot take writes (shutting down, a standby, out of disk, out of connections); the
// same rows would go in once the database is back.
//
extern int timescaleUnitsExec(PGconn* conn, TsUnit** unitV, int unitN, bool* downP);



// -----------------------------------------------------------------------------
//
// tsUnitRows - the rows of a unit, both tables
//
extern int tsUnitRows(const TsUnit* uP);

#endif  // TIMESCALE_TIMESCALEUNIT_H_

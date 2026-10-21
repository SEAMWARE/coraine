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
// TsUnit - the TRoE rows of one event list (one request's events), rendered as SQL VALUES rows with
// the values as literals. Units are self-contained (malloc'd, nothing of the request's arena), so a
// writer thread can send them after the request is gone, and several of them in one statement: the
// rows of every unit of a batch go in ONE INSERT per table (timescaleUnitsExec).
//

#include <stdbool.h>                                      // bool
#include <stdint.h>                                       // uint64_t
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
typedef struct TsUnit
{
  struct TsUnit*  next;          // writer queue linkage
  uint64_t        seq;           // writer queue position (timescaleWriterBarrier)

  TsBuf           entityRows;    // "(...),(...)" - VALUES rows of troe_entities
  int             entityRowN;
  TsBuf           attrRows;      // "(...),(...)" - VALUES rows of troe_attrs
  int             attrRowN;
  bool            oom;           // a row did not fit in memory - the unit is not sent

  bool            wait;          // the submitter waits for the commit (--troeSync)
  bool            done;          // set by the writer, under its mutex, for a waited-for unit
  int             rc;            // TROE_OK / TROE_ERR, valid once done
} TsUnit;



// -----------------------------------------------------------------------------
//
// tsBufText - printf-append
//
extern bool tsBufText(TsBuf* bP, const char* fmt, ...) __attribute__((format(printf, 2, 3)));



// -----------------------------------------------------------------------------
//
// tsBufLiteral - append s as a single-quoted SQL literal, cast to `cast` when given; NULL is NULL
//
// Only quotes are doubled: every connection runs with standard_conforming_strings on
// (timescaleConnect), where a backslash is an ordinary character.
//
extern bool tsBufLiteral(TsBuf* bP, const char* s, const char* cast);



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
extern int timescaleUnitsExec(PGconn* conn, TsUnit** unitV, int unitN);

#endif  // TIMESCALE_TIMESCALEUNIT_H_

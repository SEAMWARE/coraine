#ifndef TIMESCALE_TIMESCALEEVENT_H_
#define TIMESCALE_TIMESCALEEVENT_H_

//
// FILE            timescaleEvent.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Write TroeEvents to postgres. The dispatch layer hands us the whole
// request's queue in one call (troeDispatch prefers a driver's list hook
// over its per-event hooks, and this driver offers only the list one, so
// a request's events share one transaction); the entry point acquires a
// connection from the tenant's pool and assigns it to the thread-local
// timescaleConn.
//

#include "troe/TroeDriver.h"                              // TroeEvent


extern int timescaleEventList  (const TroeEvent* listHead, int count);

//
// Row-insert primitives — operate on the thread-local timescaleConn (set by
// the calling entry point from the tenant's pool). Used by the temporal-write
// endpoints to insert direct historical rows (multiple rows per request, all
// inside one transaction).
//
extern int timescaleExecEntityInsertLocked(const TroeEvent* evP);
extern int timescaleExecAttrInsertLocked  (const TroeEvent* evP);

//
// Format epoch nanoseconds as a postgres "to_timestamp(<seconds>)" SQL
// fragment for inlining into INSERT/UPDATE statements.
//
extern void timescaleNsToSqlTimestamp(uint64_t ns, char* buf, int bufSize);


// -----------------------------------------------------------------------------
//
// troeTypeNameQuoted - append "name" to a postgres text[] array literal being
// built in buf, quote- and backslash-escaped. Returns the new write position.
//
extern int troeTypeNameQuoted(const char* name, char* buf, int pos, int bufSize);

#endif  // TIMESCALE_TIMESCALEEVENT_H_

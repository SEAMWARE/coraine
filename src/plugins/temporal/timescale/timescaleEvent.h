#ifndef TIMESCALE_TIMESCALEEVENT_H_
#define TIMESCALE_TIMESCALEEVENT_H_

//
// FILE            timescaleEvent.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Write a TroeEvent to postgres. The dispatch layer hands us one event
// at a time (or a list); each call takes the timescaleMutex.
//

#include "troe/TroeDriver.h"                              // TroeEvent


extern int timescaleEntityEvent(const TroeEvent* evP);
extern int timescaleAttrEvent  (const TroeEvent* evP);
extern int timescaleEventList  (const TroeEvent* listHead, int count);

//
// Lock-not-held insert primitives — caller must hold timescaleMutex.
// Used by the temporal-write endpoints to insert direct historical
// rows (multiple rows per request, all inside one transaction).
//
extern int timescaleExecEntityInsertLocked(const TroeEvent* evP);
extern int timescaleExecAttrInsertLocked  (const TroeEvent* evP);

//
// Format epoch nanoseconds as a postgres "to_timestamp(<seconds>)" SQL
// fragment for inlining into INSERT/UPDATE statements.
//
extern void timescaleNsToSqlTimestamp(uint64_t ns, char* buf, int bufSize);

#endif  // TIMESCALE_TIMESCALEEVENT_H_

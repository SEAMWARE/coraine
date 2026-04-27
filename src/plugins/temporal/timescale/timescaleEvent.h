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

#endif  // TIMESCALE_TIMESCALEEVENT_H_

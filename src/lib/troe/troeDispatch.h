#ifndef TROE_TROEDISPATCH_H_
#define TROE_TROEDISPATCH_H_

//
// FILE            troeDispatch.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Per-thread defer queue for TRoE events. Service routines call
// troeDeferEntityEvent / troeDeferAttrEvent during the local DB write
// (after the distop chop, so only locally-applied changes enter the queue).
// troeDispatchPending() drains the queue post-response, alongside metrics
// and ldNotifyDispatchPending.
//

#include "troe/TroeDriver.h"                              // TroeEvent, TroeOp


//
// Append events to the per-thread queue. The TroeEvent and any pointers it
// carries must live until the post-response dispatch (allocate from the
// per-request kalloc arena).
//
extern void troeDeferEntityEvent(const TroeEvent* evP);
extern void troeDeferAttrEvent(const TroeEvent* evP);


//
// Drain the per-thread queue into the loaded plugin. Called from
// brokerPostResponseHook after the response is delivered. No-op when the
// queue is empty or no plugin is loaded.
//
extern void troeDispatchPending(void);

#endif  // TROE_TROEDISPATCH_H_

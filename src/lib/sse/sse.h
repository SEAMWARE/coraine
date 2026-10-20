#ifndef SRC_LIB_SSE_SSE_H_
#define SRC_LIB_SSE_SSE_H_

//
// FILE            sse.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Server-Sent Events - a subscription made on its own event stream (doc/sse.md)
//
// POST /ngsi-ld/v1/subscriptions (or /csourceSubscriptions) with the endpoint urn:ngsi-ld:self: the 201
// is a text/event-stream response that does not end, and the subscription's
// notifications are its events. The subscription lives as long as the stream: the stream ends - the
// client left, or fell too far behind - and the subscription is deleted; one a killed broker left in the
// store is deleted at the next start. COR_FEATURE_SSE.
//
#include <stdbool.h>                                 // bool

#include "corTree/CorNode.h"                         // CorNode
#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// sseInit - the hook corNgsild delivers on (ldNotifyStreamSet); at most 'streamsMax' streams open at a
// time (--sseStreams). The thread that keeps the streams starts with the first one.
//
extern bool sseInit(int streamsMax);



// -----------------------------------------------------------------------------
//
// sseEndpointIs - the subscription's notification.endpoint.uri is urn:ngsi-ld:self
//
extern bool sseEndpointIs(CorNode* subP);



// -----------------------------------------------------------------------------
//
// sseStreamCreate - this request may make a subscription on its stream (POST /subscriptions or
// /csourceSubscriptions, `Accept: text/event-stream`): the Accept precondition (406) waits for the body -
// the route checks Accept itself (sseCreateCheck)
//
extern bool sseStreamCreate(void);



// -----------------------------------------------------------------------------
//
// sseCreateCheck - before a subscription is stored: is it one on its stream, and may it be made?
//
// *streamP: it is (the endpoint urn:ngsi-ld:self). false: it may not be, and the error is set - an
// Accept without text/event-stream, text/* or */* (406; for one not on a stream, the usual Accept
// precondition), a request that cannot carry a stream (400: over a WebSocket, cor://), --sseStreams
// streams open already or the broker stopping (503).
// A stream that may be made has its place reserved: sseCreateRegister or sseCreateAbort follows.
//
extern bool sseCreateCheck(CorNode* subP, bool* streamP);



// -----------------------------------------------------------------------------
//
// sseCreateAbort - the subscription was not made after all: its place is free again
//
extern void sseCreateAbort(void);



// -----------------------------------------------------------------------------
//
// sseCreateRegister - the subscription is stored: its stream is there for its notifications, before it is
// in the cache (a notification may come at once) - they wait until the stream is open
//
extern void sseCreateRegister(Tenant* tenantP, const char* subId, bool csrSub);



// -----------------------------------------------------------------------------
//
// sseCreateRespond - the 201 is the stream: its headers, and the response handed to corRest
//
extern void sseCreateRespond(Tenant* tenantP, const char* subId, bool csrSub);



// -----------------------------------------------------------------------------
//
// sseForget - the subscription is deleted: its stream ends
//
extern void sseForget(Tenant* tenantP, const char* subId, bool csrSub);



// -----------------------------------------------------------------------------
//
// sseLeftoversDelete - the subscriptions on an event stream in the store, deleted: no stream outlives the
// broker that served it - they are a killed broker's. At the start, not with --high-availability.
//
extern void sseLeftoversDelete(void);



// -----------------------------------------------------------------------------
//
// sseStop - every stream ended and its subscription deleted; no new one from here on. Before corRestStop.
//
extern void sseStop(void);

#endif  // SRC_LIB_SSE_SSE_H_

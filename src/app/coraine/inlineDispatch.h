#ifndef SRC_APP_CORAINE_INLINEDISPATCH_H_
#define SRC_APP_CORAINE_INLINEDISPATCH_H_

//
// FILE            inlineDispatch.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                   // bool



// -----------------------------------------------------------------------------
//
// inlineDispatchInit - let the requests that wait on nothing run on the I/O thread that read them
//
// Every request used to be handed from its I/O thread to a worker and back: two thread switches,
// which for a corDB request cost more than the request itself (retrieve: ~93k cycles per request
// with the hop, ~50k without - x2.2 throughput on 4 cores). The hop is there so that a request that
// WAITS does not stop the other connections of its I/O thread, and what can wait is decided here:
//
//   once, at startup - the stores. Only with corDB as the current-state store and TRoE 'none' or
//   'corDB' can a request avoid waiting; otherwise the hook is not even installed and nothing
//   changes (mongoc and timescale are a network round trip on every write).
//
//   per request (inlineDispatchCheck) - a request of the /ngsi-ld/v1/entities family; with
//   --distributed, no registration anywhere (without it, nothing is ever forwarded); not waiting
//   for a bridge service's reply (?ddsSync, --ddsSync); and an @context that is already cached
//   (a download would be seconds on the I/O thread): the Link header's, else the default user
//   context - or, for application/ld+json, the body's, found by a light scan of the raw body (a
//   URL, an array of them, an inline object that neither imports nor scopes one). Anything else
//   is handed off, as before.
//
// Cautious on purpose: a request wrongly kept here stalls its thread's other connections, one
// wrongly handed off only costs what every request cost before.
//
// The built-in server (corHttp) is also asked, once the response is sent, whether the request's
// post-response phase may run on its loop thread: yes when the request left nothing in it that could
// wait (no notification, CSR notification, bridge release, expired entity or registration probe) -
// which is every read and most writes. That second hop cost corHttp two thirds of its throughput.
//
// disabled: --noInline - every request handed off (measurements, and a way out).
//
extern void inlineDispatchInit(const char* dbName, const char* troeName, bool disabled);



// -----------------------------------------------------------------------------
//
// inlineDispatchNeverWaits - is the store one that never waits on I/O (corDB, ramDB, with TRoE none or
// corDB) - known after inlineDispatchInit
//
extern bool inlineDispatchNeverWaits(void);

#endif  // SRC_APP_CORAINE_INLINEDISPATCH_H_

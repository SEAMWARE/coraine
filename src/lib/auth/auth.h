#ifndef CORAINE_AUTH_H_
#define CORAINE_AUTH_H_

//
// FILE            auth.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The broker's side of the auth plugin (plugin/AuthDriver.h, doc/authorization.md): loading it, asking it
// whether a request comes in (corRest's admit hook) and whether it may do what it asks (the pre-service
// hook), and the 401 / 403 / 503 when not. Without --auth none of it runs: no hook is set.
//
#include <stdbool.h>                                 // bool

#include "corArgs/corArgs.h"                         // CorArg



// -----------------------------------------------------------------------------
//
// authLoad - the plugin of --auth (a name in <plugins>/auth/, or a path); 0 on success
//
extern int authLoad(const char* name, char* errorBuf, int errorBufSize);



// -----------------------------------------------------------------------------
//
// authArgs - the loaded plugin's own options, NULL: none (or no plugin)
//
extern CorArg* authArgs(void);



// -----------------------------------------------------------------------------
//
// authPluginName - the loaded plugin's name ("fdsc"), NULL: none
//
extern const char* authPluginName(void);



// -----------------------------------------------------------------------------
//
// authInit - the plugin initialised and corRest's admit hook set; false: it refused - the broker must not
// start (fail closed). true with no plugin loaded.
//
extern bool authInit(void);



// -----------------------------------------------------------------------------
//
// authAuthorize - may the request do what it asks: the pre-service hook's, once the @context is resolved
// and BEFORE the tenant is (resolving it may create it). false: the error is set (403 / 503).
//
extern bool authAuthorize(void);



// -----------------------------------------------------------------------------
//
// authRelease - the request is over: its identity back to the plugin (the post-response hook)
//
extern void authRelease(void);



// -----------------------------------------------------------------------------
//
// authTrustNext - the next request THIS thread runs is the broker's own (corRestRunJson, e.g. deleting a
// closed WebSocket's subscriptions): let in without asking. Consumed by that request's admission - call
// it immediately before the corRestRunJson.
//
extern void authTrustNext(void);



// -----------------------------------------------------------------------------
//
// authTrustClear - authTrustNext's mark dropped, consumed or not - call it right after the corRestRunJson:
// a run that never reached admission must not leave the next request on this thread trusted
//
extern void authTrustClear(void);



// -----------------------------------------------------------------------------
//
// authVersionInfo - the plugin's line for GET /version, NULL: no plugin
//
extern const char* authVersionInfo(void);



// -----------------------------------------------------------------------------
//
// authClose - the broker stops
//
extern void authClose(void);

#endif  // CORAINE_AUTH_H_

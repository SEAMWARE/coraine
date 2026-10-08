#ifndef STARTUP_STARTUPCONTEXT_H_
#define STARTUP_STARTUPCONTEXT_H_

//
// FILE            startupContext.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The callbacks corLdInit takes, for every program built on the broker's libraries (the broker,
// coraine-import)
//



// -----------------------------------------------------------------------------
//
// startupContextDownload - CorLdDownloadFunction: a remote @context fetched (the body malloc'd - corJsonld frees it)
//
extern char* startupContextDownload(const char* url, int* statusCodeP);



// -----------------------------------------------------------------------------
//
// startupContextError - CorLdErrorFunction: an @context the library can NAME
//
// corLdContextFromUrl answers NULL for everything, and its callers turn that into "unable to retrieve
// @context" - true for a download that failed, wrong for one that downloaded perfectly and is unusable.
// The library reports the ones it can name here, and this turns them into the ProblemDetails the client
// sees. corNgsild.contextError is what stops the caller from then overwriting it with its own generic
// answer.
//
extern void startupContextError(int status, const char* title, const char* detail);

#endif  // STARTUP_STARTUPCONTEXT_H_

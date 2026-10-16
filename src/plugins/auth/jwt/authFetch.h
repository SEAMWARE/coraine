#ifndef AUTH_FETCH_H_
#define AUTH_FETCH_H_

//
// FILE            authFetch.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corAlloc/CorAlloc.h"                       // CorAlloc



// -----------------------------------------------------------------------------
//
// authFetch - the text behind a URL (http://, https://) or in a file (a path, or file://), in kaP.
// NULL: not there - errorBuf says why. NEVER on a request's path: it blocks.
//
extern char* authFetch(const char* where, CorAlloc* kaP, char* errorBuf, int errorBufSize);

#endif  // AUTH_FETCH_H_

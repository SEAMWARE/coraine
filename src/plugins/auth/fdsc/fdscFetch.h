#ifndef FDSC_FETCH_H_
#define FDSC_FETCH_H_

//
// FILE            fdscFetch.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corAlloc/CorAlloc.h"                       // CorAlloc



// -----------------------------------------------------------------------------
//
// fdscFetch - the text behind a URL (http://, https://) or in a file (a path, or file://), in kaP.
// NULL: not there - errorBuf says why. NEVER on a request's path: it blocks.
//
extern char* fdscFetch(const char* where, CorAlloc* kaP, char* errorBuf, int errorBufSize);

#endif  // FDSC_FETCH_H_

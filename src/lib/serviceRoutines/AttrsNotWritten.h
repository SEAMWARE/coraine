#ifndef ATTRS_NOT_WRITTEN_H_
#define ATTRS_NOT_WRITTEN_H_

//
// FILE            AttrsNotWritten.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// AttrsNotWritten - the attributes a write request takes out of what it stores, and why
//
// A request writing SEVERAL attributes does not fail as a whole because one of them was refused before
// the write: that attribute is taken out of the fragment and not written, the rest is, and the handler
// reports it as not updated (207) - or, where the route keeps what was stored, keeps it. ONE attribute:
// the request fails with the refusal's error instead, and nothing lands here.
//
// The list belongs to the REQUEST, not to whatever refused: each step before the write adds what it
// refused, with its own status and reason, and the handler reads it once. Lives on the handler's stack
// (a batch: one per fragment, in the request's arena).
//
#include <stdbool.h>                                  // bool



// -----------------------------------------------------------------------------
//
// AttrsNotWritten -
//
typedef struct AttrsNotWritten
{
  int          n;
  const char*  attrV[16];                             // the attribute - not written
  int          statusV[16];                           // what a request of it alone would have answered
  const char*  reasonV[16];                           // why, in the request's arena
  const char*  titleV[16];                            // the ProblemDetails title of that status
  const char*  typeV[16];                             // ... and its type
} AttrsNotWritten;



// -----------------------------------------------------------------------------
//
// attrsNotWrittenAdd - one more; 'reason' is copied into the request's arena. false: the list is full
//
extern bool attrsNotWrittenAdd(AttrsNotWritten* listP, const char* attr, int status, const char* type, const char* title, const char* reason);

#endif  // ATTRS_NOT_WRITTEN_H_

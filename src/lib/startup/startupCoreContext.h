#ifndef STARTUP_STARTUPCORECONTEXT_H_
#define STARTUP_STARTUPCORECONTEXT_H_

//
// FILE            startupCoreContext.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corAlloc/CorAlloc.h"                            // CorAlloc



// -----------------------------------------------------------------------------
//
// startupCoreContext - JSON-LD and NGSI-LD, set up as every program built on the broker's libraries
// needs them (the broker, coraine-import)
//
// corLdInit (startupContext.h's callbacks), the terms the broker adds to the core context, every core
// term's CorTerm id, ldInit. contextAllocP: the store of the @contexts, for the life of the program.
// NULL: done; otherwise what failed.
//
extern const char* startupCoreContext(CorAlloc* contextAllocP);

#endif  // STARTUP_STARTUPCORECONTEXT_H_

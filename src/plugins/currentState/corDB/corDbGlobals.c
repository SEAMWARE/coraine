//
// FILE            corDbGlobals.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "kargs/KArg.h"                              // KArg

#include "currentState/corDB/corDbGlobals.h"           // Own interface



// -----------------------------------------------------------------------------
//
// corDbArgV - no CLI args for the corDb plugin (in-memory, no DB connection)
//
KArg* corDbArgV = NULL;

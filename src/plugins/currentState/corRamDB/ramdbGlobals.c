//
// FILE            ramdbGlobals.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kargs/KArg.h"                              // KArg

#include "currentState/corRamDB/ramdbGlobals.h"           // Own interface



// -----------------------------------------------------------------------------
//
// ramdbArgV - no CLI args for the ramdb plugin (in-memory, no DB connection)
//
KArg* ramdbArgV = NULL;

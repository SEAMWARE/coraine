//
// FILE            startupArena.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                       // NULL

#include "corAlloc/corAllocBufferInit.h"                  // corAllocBufferInit
#include "corJson/corJsonCreate.h"                        // corJsonCreate
#include "corRest/CorRestState.h"                         // corRest

#include "startup/startupArena.h"                         // Own interface



// -----------------------------------------------------------------------------
//
// startupArena -
//
void startupArena(void)
{
  static char startupKallocBuf[16384];

  corAllocBufferInit(&corRest.kalloc, startupKallocBuf, sizeof(startupKallocBuf), 4096, NULL, "startup");
  corRest.corJsonP = corJsonCreate(&corRest.corJson, &corRest.kalloc);
  corRest.kallocP  = &corRest.kalloc;
}

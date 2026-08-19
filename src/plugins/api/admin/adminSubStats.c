//
// FILE            adminSubStats.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                   // NULL

#include "corRest/CorRestState.h"                       // corRest

#include "metrics/subStatsFlushAll.h"                 // subStatsFlushAll

#include "api/admin/adminSubStats.h"                  // Own interface



// -----------------------------------------------------------------------------
//
// adminPostSubStatsFlush - thin wrapper around subStatsFlushAll.
//
// The actual walker lives in broker core so the periodic timer can
// call it too without duplicating code.
//
bool adminPostSubStatsFlush(void)
{
  subStatsFlushAll();
  corRest.out.httpStatusCode = 204;
  return true;
}

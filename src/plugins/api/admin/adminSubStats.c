//
// FILE            adminSubStats.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stddef.h>                                   // NULL

#include "swRest/SwRestState.h"                       // swRest

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
  swRest.out.httpStatusCode = 204;
  return true;
}

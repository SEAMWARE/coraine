//
// FILE            timescaleInit.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                                      // bool
#include <stddef.h>                                       // NULL
#include <unistd.h>                                       // sleep

#include "corLog/corLog.h"                                // COR_E, COR_W, COR_I
#include "corArgs/corArgsSecret.h"                // corArgsSecret

#include "db/Tenant.h"                                    // tenant0

#include "troe/TroeDriver.h"                              // TROE_OK, TROE_ERR

#include "temporal/timescale/timescaleGlobals.h"          // timescaleDb*
#include "temporal/timescale/timescalePool.h"             // timescalePoolEnsureReach, timescalePoolCloseAll
#include "temporal/timescale/timescaleMigrate.h"          // timescaleIndexesParse
#if COR_FEATURE_DB_INDEXES
#include "temporal/timescale/timescaleIndexes.h"          // timescaleIndexesInit, timescaleIndexesLoad
#endif
#include "temporal/timescale/timescaleInit.h"             // Own interface



// -----------------------------------------------------------------------------
//
// timescaleInit - build the default tenant's database, migrate it and open
// its pool.
//
// Per-tenant databases are created lazily on first use (timescaleConnGet ->
// timescalePoolEnsure). The default tenant is built eagerly here so a fresh
// broker surfaces any Postgres connectivity / permission problem at boot
// rather than on the first temporal request.
//
int timescaleInit(void)
{
  corArgsSecret("--troePwd");                      // never shown: GET /admin/config masks it

  if (timescaleIndexesParse(timescaleIndexes) != 0)
    return TROE_ERR;

#if COR_FEATURE_DB_INDEXES
  if (timescaleIndexesInit() != 0)                  // --troeDbIndexes: before the default tenant's pool applies them
    return TROE_ERR;
#endif

  //
  // A database that is not there yet - started together with the broker, restarting - is waited for,
  // as mongoc waits for its server (--dbTimeout): a try a second, for --troeConnectTimeout seconds.
  // One that is there and refuses (a schema version this build does not know) is not: a try later
  // finds it the same.
  //
  bool unreachable;

  for (int tryNo = 0; timescalePoolEnsureReach(&tenant0, &unreachable) != TROE_OK; tryNo++)
  {
    if (unreachable == false)
    {
      COR_E("timescale: failed to initialise the default-tenant database");
      return TROE_ERR;
    }

    if (tryNo >= timescaleConnectTimeout)
    {
      COR_E("timescale: failed to initialise the default-tenant database (%d s, --troeConnectTimeout)", timescaleConnectTimeout);
      return TROE_ERR;
    }

    if (tryNo == 0)
      COR_W("timescale: the history database cannot be reached - trying again once a second, for %d s (--troeConnectTimeout)", timescaleConnectTimeout);

    sleep(1);
  }

#if COR_FEATURE_DB_INDEXES
  if (timescaleIndexesLoad() != 0)                  // the admin API's: kept in the default tenant's database
    return TROE_ERR;
#endif

  COR_I("timescale: ready (%s:%d, base db '%s', pool size %d)",
        timescaleDbHost ? timescaleDbHost : "localhost",
        timescaleDbPort,
        timescaleDbName ? timescaleDbName : "corh",
        timescalePoolSize);

  return TROE_OK;
}



// -----------------------------------------------------------------------------
//
// timescaleClose - finish every pooled connection across all tenants.
//
void timescaleClose(void)
{
#if COR_FEATURE_HEALTH
  timescalePingClose();
#endif
  timescalePoolCloseAll();
}

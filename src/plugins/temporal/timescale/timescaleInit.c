//
// FILE            timescaleInit.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stddef.h>                                       // NULL

#include "ktrace/kTrace.h"                                // KT_E, KT_I

#include "db/Tenant.h"                                    // tenant0

#include "troe/TroeDriver.h"                              // TROE_OK, TROE_ERR

#include "temporal/timescale/timescaleGlobals.h"          // timescaleDb*
#include "temporal/timescale/timescalePool.h"             // timescalePoolEnsure, timescalePoolCloseAll
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
  if (timescalePoolEnsure(&tenant0) != TROE_OK)
  {
    KT_E("timescale: failed to initialise the default-tenant database");
    return TROE_ERR;
  }

  KT_I("timescale: ready (%s:%d, base db '%s', pool size %d)",
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
  timescalePoolCloseAll();
}

//
// FILE            timescaleInit.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stdio.h>                                        // snprintf
#include <stddef.h>                                       // NULL
#include <libpq-fe.h>                                     // PG*

#include "ktrace/kTrace.h"                                // KT_E, KT_I

#include "troe/TroeDriver.h"                              // TROE_OK, TROE_ERR

#include "temporal/timescale/timescaleGlobals.h"          // timescaleConn, timescaleDb*
#include "temporal/timescale/timescaleMigrate.h"          // timescaleMigrate
#include "temporal/timescale/timescaleInit.h"             // Own interface



// -----------------------------------------------------------------------------
//
// timescaleInit - connect + run pending migrations.
//
int timescaleInit(void)
{
  char connStr[1024];

  if (timescaleDbPwd != NULL)
  {
    snprintf(connStr, sizeof(connStr),
             "host=%s port=%d dbname=%s user=%s password=%s",
             timescaleDbHost ? timescaleDbHost : "localhost",
             timescaleDbPort,
             timescaleDbName ? timescaleDbName : "sw_troe",
             timescaleDbUser ? timescaleDbUser : "postgres",
             timescaleDbPwd);
  }
  else
  {
    snprintf(connStr, sizeof(connStr),
             "host=%s port=%d dbname=%s user=%s",
             timescaleDbHost ? timescaleDbHost : "localhost",
             timescaleDbPort,
             timescaleDbName ? timescaleDbName : "sw_troe",
             timescaleDbUser ? timescaleDbUser : "postgres");
  }

  timescaleConn = PQconnectdb(connStr);
  if (PQstatus(timescaleConn) != CONNECTION_OK)
  {
    KT_E("timescale: postgres connection failed: %s", PQerrorMessage(timescaleConn));
    PQfinish(timescaleConn);
    timescaleConn = NULL;
    return TROE_ERR;
  }

  KT_I("timescale: connected to %s:%d/%s",
       timescaleDbHost ? timescaleDbHost : "localhost",
       timescaleDbPort,
       timescaleDbName ? timescaleDbName : "sw_troe");

  if (timescaleMigrate() != 0)
  {
    KT_E("timescale: schema migration failed");
    PQfinish(timescaleConn);
    timescaleConn = NULL;
    return TROE_ERR;
  }

  return TROE_OK;
}



// -----------------------------------------------------------------------------
//
// timescaleClose -
//
void timescaleClose(void)
{
  if (timescaleConn != NULL)
  {
    PQfinish(timescaleConn);
    timescaleConn = NULL;
  }
}

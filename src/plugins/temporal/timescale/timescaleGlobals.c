//
// FILE            timescaleGlobals.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                       // NULL

#include "corArgs/corArgs.h"                              // CORARGS_END
#include "corArgs/CorArg.h"                               // CorArg

#include "temporal/timescale/timescaleGlobals.h"          // Own interface



// timescaleConn - the connection the current thread is operating on. Each
// entry point acquires a connection from its tenant's pool and assigns it
// here for the duration of the call; every SQL helper reads it. Thread-local,
// so concurrent requests (different tenants or different pool slots of the
// same tenant) never share a connection — which is what makes the old global
// mutex unnecessary.
__thread PGconn* timescaleConn   = NULL;

char*            timescaleDbHost      = NULL;
char*            timescaleDbName      = NULL;
char*            timescaleDbUser      = NULL;
char*            timescaleDbPwd       = NULL;
int              timescaleDbPort      = 5432;
int              timescalePoolSize    = 10;     // per-tenant connection pool size (--troePoolSize)
int              timescaleInstanceCap = 1000000;  // § 6.3.10 default temporal-instance cap
                                                  //
                                                  // Effectively a safety upper bound, not a working
                                                  // truncation knob — 100 was too small (any modest
                                                  // observed-attribute stream blows past it within
                                                  // hours and the 206 surprises clients who never
                                                  // asked for lastN). Set this large enough that
                                                  // 206 + Content-Range only fires when the operator
                                                  // explicitly tightens -troeCap.
                                                  //
                                                  // Open design question (not implemented):
                                                  // a true cap would be a rolling window — keep only
                                                  // the newest N instances at INSERT time and let
                                                  // queries return everything they find (always 200).
                                                  // That makes the cap a real memory bound instead of
                                                  // a query-time surprise. Today's cap is the latter.



// -----------------------------------------------------------------------------
//
// timescaleArgV - plugin-contributed CLI args
//
CorArg timescaleArgV[] =
{
  { "--troeHost", "-troeHost", CorArgString, _vp &timescaleDbHost, CorArgOpt, _vp "localhost", NULL, NULL, "TRoE postgres host" },
  { "--troeName", "-troeName", CorArgString, _vp &timescaleDbName, CorArgOpt, _vp "corh",   NULL, NULL, "TRoE postgres database name" },
  { "--troeUser", "-troeUser", CorArgString, _vp &timescaleDbUser, CorArgOpt, _vp "postgres",  NULL, NULL, "TRoE postgres user" },
  { "--troePwd",  "-troePwd",  CorArgString, _vp &timescaleDbPwd,  CorArgOpt, _vp NULL,    NULL, NULL, "TRoE postgres password" },
  { "--troePort", "-troePort", CorArgInt,    _vp &timescaleDbPort, CorArgOpt, _vp 5432,    _vp 1, _vp 65535, "TRoE postgres port" },
  { "--troePoolSize", "-troePoolSize", CorArgInt, _vp &timescalePoolSize, CorArgOpt, _vp 10,   _vp 1, _vp 256, "TRoE per-tenant postgres connection-pool size" },
  { "--troeInstanceCap", "-troeCap", CorArgInt, _vp &timescaleInstanceCap, CorArgOpt, _vp 1000000, _vp 1, _vp 1000000, "Default per-attribute temporal page limit when ?firstN/?lastN absent (§ 6.4.7.3)" },
  CORARGS_END
};

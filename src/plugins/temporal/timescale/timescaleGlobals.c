//
// FILE            timescaleGlobals.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stddef.h>                                       // NULL
#include <pthread.h>                                      // pthread_mutex_t

#include "kargs/kargs.h"                                  // KARGS_END
#include "kargs/KArg.h"                                   // KArg

#include "temporal/timescale/timescaleGlobals.h"          // Own interface



PGconn*          timescaleConn   = NULL;
pthread_mutex_t  timescaleMutex  = PTHREAD_MUTEX_INITIALIZER;

char*            timescaleDbHost      = NULL;
char*            timescaleDbName      = NULL;
char*            timescaleDbUser      = NULL;
char*            timescaleDbPwd       = NULL;
int              timescaleDbPort      = 5432;
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
KArg timescaleArgV[] =
{
  { "--troeHost", "-troeHost", KaString, _vp &timescaleDbHost, KaOpt, _vp "localhost", NULL, NULL, "TRoE postgres host" },
  { "--troeName", "-troeName", KaString, _vp &timescaleDbName, KaOpt, _vp "sw_troe",   NULL, NULL, "TRoE postgres database name" },
  { "--troeUser", "-troeUser", KaString, _vp &timescaleDbUser, KaOpt, _vp "postgres",  NULL, NULL, "TRoE postgres user" },
  { "--troePwd",  "-troePwd",  KaString, _vp &timescaleDbPwd,  KaOpt, _vp NULL,        NULL, NULL, "TRoE postgres password" },
  { "--troePort", "-troePort", KaInt,    _vp &timescaleDbPort, KaOpt, _vp 5432,        _vp 1, _vp 65535, "TRoE postgres port" },
  { "--troeInstanceCap", "-troeCap", KaInt, _vp &timescaleInstanceCap, KaOpt, _vp 1000000, _vp 1, _vp 1000000, "Per-entity attribute-instance cap before 206 + Content-Range (§ 6.3.10)" },
  KARGS_END
};

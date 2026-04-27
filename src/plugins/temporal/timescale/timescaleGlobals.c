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

char*            timescaleDbHost = NULL;
char*            timescaleDbName = NULL;
char*            timescaleDbUser = NULL;
char*            timescaleDbPwd  = NULL;
int              timescaleDbPort = 5432;



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
  KARGS_END
};

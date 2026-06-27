#ifndef TIMESCALE_TIMESCALEGLOBALS_H_
#define TIMESCALE_TIMESCALEGLOBALS_H_

//
// FILE            timescaleGlobals.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Shared state for the timescale TRoE plugin. Each tenant owns its own
// physical database and a bounded connection pool (see timescalePool.h);
// timescaleConn is the per-thread connection currently in use, acquired
// from the tenant's pool at the entry point.
//

#include <libpq-fe.h>                                     // PGconn

#include "kargs/KArg.h"                                   // KArg


// Per-thread connection in use by the current entry-point call. Set from the
// tenant's pool at the top of each entry point; every SQL helper reads it.
extern __thread PGconn*  timescaleConn;

extern char*             timescaleDbHost;
extern char*             timescaleDbName;
extern char*             timescaleDbUser;
extern char*             timescaleDbPwd;
extern int               timescaleDbPort;
extern int               timescalePoolSize;
extern int               timescaleInstanceCap;

extern KArg              timescaleArgV[];

#endif  // TIMESCALE_TIMESCALEGLOBALS_H_

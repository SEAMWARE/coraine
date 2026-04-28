#ifndef TIMESCALE_TIMESCALEGLOBALS_H_
#define TIMESCALE_TIMESCALEGLOBALS_H_

//
// FILE            timescaleGlobals.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Shared state for the timescale TRoE plugin: a single PGconn protected
// by a mutex. Sufficient for v1 — workload is one or a few INSERTs per
// request, drained post-response. Move to a connection pool when wrk
// shows the mutex is the bottleneck.
//

#include <pthread.h>                                      // pthread_mutex_t
#include <libpq-fe.h>                                     // PGconn

#include "kargs/KArg.h"                                   // KArg


extern PGconn*           timescaleConn;
extern pthread_mutex_t   timescaleMutex;

extern char*             timescaleDbHost;
extern char*             timescaleDbName;
extern char*             timescaleDbUser;
extern char*             timescaleDbPwd;
extern int               timescaleDbPort;
extern int               timescaleInstanceCap;

extern KArg              timescaleArgV[];

#endif  // TIMESCALE_TIMESCALEGLOBALS_H_

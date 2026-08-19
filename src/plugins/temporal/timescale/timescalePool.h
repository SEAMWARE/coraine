#ifndef TIMESCALE_TIMESCALEPOOL_H_
#define TIMESCALE_TIMESCALEPOOL_H_

//
// FILE            timescalePool.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Per-tenant Postgres connection pool for the timescale TRoE plugin.
//
// Each tenant owns its own physical database ("corh" for the default
// tenant, "corh_<tenant>" otherwise) and its own bounded pool of
// connections. The pool hangs off Tenant.troePoolP — there is no global
// registry to search or lock, only a single create-mutex guarding the
// lazy first-create of a tenant's pool.
//
// A Postgres connection is bound to its database at connect time (unlike
// Mongo), so per-tenant databases require per-tenant connections — hence
// a pool per tenant rather than one shared pool.
//

#include <stdbool.h>                                      // bool
#include <stdint.h>                                       // uint64_t
#include <semaphore.h>                                    // sem_t
#include <libpq-fe.h>                                     // PGconn

#include "db/Tenant.h"                                    // Tenant



// -----------------------------------------------------------------------------
//
// TimescaleConn - one pooled connection slot.
//
typedef struct TimescaleConn
{
  bool                   busy;     // in use by a thread right now
  PGconn*                conn;     // NULL until first connected (lazy)
  uint64_t               uses;     // acquire count (diagnostics)
  struct TimescalePool*  poolP;    // owning pool (back-pointer for release)
} TimescaleConn;



// -----------------------------------------------------------------------------
//
// TimescalePool - a tenant's bounded connection pool. Hangs off Tenant.troePoolP.
//
typedef struct TimescalePool
{
  char            dbName[128];     // physical postgres database for this tenant
  sem_t           queueSem;        // counting: number of free slots (init = items)
  sem_t           poolSem;         // binary: guards connV slot selection
  int             items;           // pool size (--troePoolSize)
  TimescaleConn*  connV;           // allocated array[items]
} TimescalePool;



// -----------------------------------------------------------------------------
//
// timescaleDbNameFor - physical database name for a tenant into buf.
//
extern void           timescaleDbNameFor(Tenant* tenantP, char* buf, int bufSize);



// -----------------------------------------------------------------------------
//
// timescalePoolEnsure - lazily CREATE the tenant's database, migrate it and
// build its pool. Idempotent — a no-op once the pool exists.
//
extern int            timescalePoolEnsure(Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// timescaleConnGet - acquire a busy connection for the tenant (ensures the
// pool first). Bounded wait; returns NULL on timeout / connection error.
//
extern TimescaleConn* timescaleConnGet(Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// timescaleConnRelease - release a connection acquired via timescaleConnGet.
// Resets any leftover transaction state before handing the slot back.
//
extern void           timescaleConnRelease(TimescaleConn* cP);



// -----------------------------------------------------------------------------
//
// timescalePoolDrop - DROP the tenant's database and free its pool.
// No-op for the default tenant (never drop the live broker's data).
//
extern int            timescalePoolDrop(Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// timescalePoolCloseAll - finish every pooled connection across all tenants.
// Called at plugin shutdown.
//
extern void           timescalePoolCloseAll(void);

#endif  // TIMESCALE_TIMESCALEPOOL_H_

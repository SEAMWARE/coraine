//
// FILE            timescalePool.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Per-tenant Postgres connection pool — see timescalePool.h.
//

#include <stddef.h>                                       // NULL
#include <stdio.h>                                        // snprintf
#include <stdlib.h>                                       // calloc, free
#include <string.h>                                       // strcmp, strlen, memcpy
#include <time.h>                                         // clock_gettime
#include <pthread.h>                                      // pthread_mutex_t
#include <semaphore.h>                                    // sem_*
#include <libpq-fe.h>                                     // PG*

#include "ktrace/kTrace.h"                                // KT_E, KT_I

#include "db/Tenant.h"                                    // Tenant, tenant0, tenantList

#include "troe/TroeDriver.h"                              // TROE_OK, TROE_ERR

#include "temporal/timescale/timescaleGlobals.h"          // timescaleDb*, timescalePoolSize
#include "temporal/timescale/timescaleMigrate.h"          // timescaleMigrate
#include "temporal/timescale/timescalePool.h"             // Own interface



// -----------------------------------------------------------------------------
//
// Seconds a thread waits for a free connection before giving up. The pool is
// bounded (--troePoolSize); under sustained overload a request fails fast
// rather than hanging forever (orion-ld's pool blocked indefinitely here).
//
#define TIMESCALE_POOL_WAIT_SECS  10



// -----------------------------------------------------------------------------
//
// poolCreateMutex - guards the lazy first-create of a tenant's pool (and the
// teardown in timescalePoolDrop). The pool itself, once created, is reached
// directly via Tenant.troePoolP — no registry, no per-access list lock.
//
static pthread_mutex_t  poolCreateMutex = PTHREAD_MUTEX_INITIALIZER;



// -----------------------------------------------------------------------------
//
// timescaleDbNameFor - physical database name for a tenant.
//
// Default tenant (empty name) → the configured base name (e.g. "sw_troe").
// Otherwise base + "_" + sanitised-tenant. The tenant name is already
// lowercased by the broker; every run of characters outside [a-z0-9] becomes
// a single '_', and leading/trailing separators are trimmed, so the result
// is a clean, collision-resistant Postgres identifier. (A snapshot tenant
// "-_snap_3" thus maps to "<base>_snap_3", not "<base>___snap_3".)
//
void timescaleDbNameFor(Tenant* tenantP, char* buf, int bufSize)
{
  const char* base = (timescaleDbName != NULL) ? timescaleDbName : "sw_troe";

  if (tenantP == NULL || tenantP->name[0] == 0)
  {
    snprintf(buf, bufSize, "%s", base);
    return;
  }

  // Build the sanitised suffix: alnum kept, separator-runs collapsed to '_'.
  char suffix[sizeof(((Tenant*) 0)->name)];
  int  q       = 0;
  bool lastSep = true;   // start "in a separator" so leading separators drop
  for (const char* s = tenantP->name; (*s != 0) && (q < (int) sizeof(suffix) - 1); s++)
  {
    char c = *s;
    if (((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')))
    {
      suffix[q++] = c;
      lastSep = false;
    }
    else if (!lastSep)
    {
      suffix[q++] = '_';
      lastSep = true;
    }
  }
  while ((q > 0) && (suffix[q - 1] == '_'))   // trim trailing separator
    q--;
  suffix[q] = 0;

  if (q == 0)
    snprintf(buf, bufSize, "%s", base);        // degenerate name — fall back to base
  else
    snprintf(buf, bufSize, "%s_%s", base, suffix);
}



// -----------------------------------------------------------------------------
//
// connStr - assemble a libpq connection string for one database.
//
static void connStr(const char* dbName, char* buf, int bufSize)
{
  if (timescaleDbPwd != NULL)
    snprintf(buf, bufSize,
             "host=%s port=%d dbname=%s user=%s password=%s",
             timescaleDbHost ? timescaleDbHost : "localhost",
             timescaleDbPort, dbName,
             timescaleDbUser ? timescaleDbUser : "postgres",
             timescaleDbPwd);
  else
    snprintf(buf, bufSize,
             "host=%s port=%d dbname=%s user=%s",
             timescaleDbHost ? timescaleDbHost : "localhost",
             timescaleDbPort, dbName,
             timescaleDbUser ? timescaleDbUser : "postgres");
}



// -----------------------------------------------------------------------------
//
// connectTo - open a fresh connection to one database, or NULL on failure.
//
static PGconn* connectTo(const char* dbName)
{
  char cs[1024];
  connStr(dbName, cs, sizeof(cs));

  PGconn* conn = PQconnectdb(cs);
  if (PQstatus(conn) != CONNECTION_OK)
  {
    KT_E("timescale: connection to db '%s' failed: %s", dbName, PQerrorMessage(conn));
    PQfinish(conn);
    return NULL;
  }
  return conn;
}



// -----------------------------------------------------------------------------
//
// ensureDatabase - CREATE DATABASE <dbName> via the admin (postgres) database.
// "already exists" (SQLSTATE 42P04) is success — the database is shared across
// brokers and may have been created by another one (or a previous boot).
//
static int ensureDatabase(const char* dbName)
{
  PGconn* admin = connectTo("postgres");
  if (admin == NULL)
    return TROE_ERR;

  char sql[256];
  snprintf(sql, sizeof(sql), "CREATE DATABASE \"%s\"", dbName);

  PGresult* r  = PQexec(admin, sql);
  int       rc = TROE_OK;

  if (PQresultStatus(r) != PGRES_COMMAND_OK)
  {
    const char* sqlState = PQresultErrorField(r, PG_DIAG_SQLSTATE);
    if (sqlState == NULL || strcmp(sqlState, "42P04") != 0)   // not "duplicate_database"
    {
      KT_E("timescale: CREATE DATABASE '%s' failed: %s", dbName, PQerrorMessage(admin));
      rc = TROE_ERR;
    }
  }

  PQclear(r);
  PQfinish(admin);
  return rc;
}



// -----------------------------------------------------------------------------
//
// timescalePoolEnsure -
//
int timescalePoolEnsure(Tenant* tenantP)
{
  if (tenantP == NULL)
    tenantP = &tenant0;

  if (tenantP->troePoolP != NULL)   // fast path — already built
    return TROE_OK;

  pthread_mutex_lock(&poolCreateMutex);

  if (tenantP->troePoolP != NULL)   // lost the create race — fine
  {
    pthread_mutex_unlock(&poolCreateMutex);
    return TROE_OK;
  }

  char dbName[128];
  timescaleDbNameFor(tenantP, dbName, sizeof(dbName));

  // 1. Make sure the database exists.
  if (ensureDatabase(dbName) != TROE_OK)
  {
    pthread_mutex_unlock(&poolCreateMutex);
    return TROE_ERR;
  }

  // 2. Connect once and run pending migrations on that connection.
  PGconn* migConn = connectTo(dbName);
  if (migConn == NULL)
  {
    pthread_mutex_unlock(&poolCreateMutex);
    return TROE_ERR;
  }

  if (timescaleMigrate(migConn) != 0)
  {
    KT_E("timescale: schema migration failed for db '%s'", dbName);
    PQfinish(migConn);
    pthread_mutex_unlock(&poolCreateMutex);
    return TROE_ERR;
  }

  // 3. Build the pool. The migration connection is kept as the first slot.
  int items = (timescalePoolSize > 0) ? timescalePoolSize : 10;

  TimescalePool* poolP = (TimescalePool*) calloc(1, sizeof(TimescalePool));
  TimescaleConn* connV = (poolP != NULL) ? (TimescaleConn*) calloc(items, sizeof(TimescaleConn)) : NULL;
  if (poolP == NULL || connV == NULL)
  {
    if (connV != NULL) free(connV);
    if (poolP != NULL) free(poolP);
    PQfinish(migConn);
    pthread_mutex_unlock(&poolCreateMutex);
    KT_E("timescale: out of memory building pool for db '%s'", dbName);
    return TROE_ERR;
  }

  // Copy with an explicit clamp + terminator rather than strncpy: at -O2 gcc can see that
  // dbName may be exactly as long as the destination and warns (stringop-truncation), and
  // -Werror turns that into a broken Release build. The behaviour is unchanged - an
  // over-long db name is still truncated to fit.
  size_t dbNameLen = strlen(dbName);
  if (dbNameLen >= sizeof(poolP->dbName))
    dbNameLen = sizeof(poolP->dbName) - 1;
  memcpy(poolP->dbName, dbName, dbNameLen);
  poolP->dbName[dbNameLen] = 0;

  poolP->items = items;
  poolP->connV = connV;
  sem_init(&poolP->queueSem, 0, items);
  sem_init(&poolP->poolSem,  0, 1);

  for (int i = 0; i < items; i++)
    connV[i].poolP = poolP;

  connV[0].conn = migConn;          // seed slot 0 with the migration connection
  connV[0].busy = false;

  tenantP->troePoolP = poolP;

  pthread_mutex_unlock(&poolCreateMutex);

  KT_I("timescale: pool ready for db '%s' (%d connections)", dbName, items);
  return TROE_OK;
}



// -----------------------------------------------------------------------------
//
// timescaleConnGet -
//
TimescaleConn* timescaleConnGet(Tenant* tenantP)
{
  if (tenantP == NULL)
    tenantP = &tenant0;

  if (tenantP->troePoolP == NULL)
  {
    if (timescalePoolEnsure(tenantP) != TROE_OK)
      return NULL;
  }

  TimescalePool* poolP = (TimescalePool*) tenantP->troePoolP;

  // Bounded wait for a free slot.
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += TIMESCALE_POOL_WAIT_SECS;
  if (sem_timedwait(&poolP->queueSem, &ts) != 0)
  {
    KT_E("timescale: connection pool exhausted for db '%s' (waited %ds)",
         poolP->dbName, TIMESCALE_POOL_WAIT_SECS);
    return NULL;
  }

  // Pick a free slot — prefer one that is already connected and healthy.
  sem_wait(&poolP->poolSem);

  TimescaleConn* cP    = NULL;
  TimescaleConn* freeP = NULL;
  for (int i = 0; i < poolP->items; i++)
  {
    if (poolP->connV[i].busy)
      continue;
    if ((poolP->connV[i].conn != NULL) && (PQstatus(poolP->connV[i].conn) == CONNECTION_OK))
    {
      cP = &poolP->connV[i];     // ready to use
      break;
    }
    if (freeP == NULL)
      freeP = &poolP->connV[i];  // virgin or stale — fallback
  }
  if (cP == NULL)
    cP = freeP;                  // queueSem guaranteed at least one free slot

  cP->busy = true;
  sem_post(&poolP->poolSem);

  // Connect / heal outside the pool lock.
  if (cP->conn == NULL)
  {
    cP->conn = connectTo(poolP->dbName);
  }
  else if (PQstatus(cP->conn) != CONNECTION_OK)
  {
    PQreset(cP->conn);
    if (PQstatus(cP->conn) != CONNECTION_OK)
    {
      PQfinish(cP->conn);
      cP->conn = connectTo(poolP->dbName);
    }
  }

  if (cP->conn == NULL)
  {
    cP->busy = false;
    sem_post(&poolP->queueSem);
    return NULL;
  }

  cP->uses += 1;
  return cP;
}



// -----------------------------------------------------------------------------
//
// timescaleConnRelease -
//
void timescaleConnRelease(TimescaleConn* cP)
{
  if (cP == NULL)
    return;

  // Reset any leftover transaction state so a half-finished (or aborted)
  // transaction can never leak into the next request on this connection.
  if ((cP->conn != NULL) && (PQstatus(cP->conn) == CONNECTION_OK))
  {
    if (PQtransactionStatus(cP->conn) != PQTRANS_IDLE)
      PQclear(PQexec(cP->conn, "ROLLBACK"));
  }

  TimescalePool* poolP = cP->poolP;
  cP->busy = false;
  if (poolP != NULL)
    sem_post(&poolP->queueSem);
}



// -----------------------------------------------------------------------------
//
// poolFree - finish every connection and free a pool (caller holds the
// create-mutex, or is single-threaded shutdown).
//
static void poolFree(TimescalePool* poolP)
{
  if (poolP == NULL)
    return;

  for (int i = 0; i < poolP->items; i++)
  {
    if (poolP->connV[i].conn != NULL)
    {
      PQfinish(poolP->connV[i].conn);
      poolP->connV[i].conn = NULL;
    }
  }

  sem_destroy(&poolP->queueSem);
  sem_destroy(&poolP->poolSem);
  free(poolP->connV);
  free(poolP);
}



// -----------------------------------------------------------------------------
//
// timescalePoolDrop -
//
int timescalePoolDrop(Tenant* tenantP)
{
  // Never drop the default tenant's database (it holds the live broker's data,
  // never a transient snapshot tenant).
  if (tenantP == NULL || tenantP->name[0] == 0)
    return TROE_OK;

  pthread_mutex_lock(&poolCreateMutex);

  char           dbName[128];
  TimescalePool* poolP = (TimescalePool*) tenantP->troePoolP;

  if (poolP != NULL)
  {
    strncpy(dbName, poolP->dbName, sizeof(dbName) - 1);
    dbName[sizeof(dbName) - 1] = 0;
    poolFree(poolP);
    tenantP->troePoolP = NULL;
  }
  else
    timescaleDbNameFor(tenantP, dbName, sizeof(dbName));

  // DROP DATABASE on the admin connection. WITH (FORCE) terminates any
  // straggler sessions (PostgreSQL 13+).
  PGconn* admin = connectTo("postgres");
  if (admin != NULL)
  {
    char sql[256];
    snprintf(sql, sizeof(sql), "DROP DATABASE IF EXISTS \"%s\" WITH (FORCE)", dbName);
    PGresult* r = PQexec(admin, sql);
    if (PQresultStatus(r) != PGRES_COMMAND_OK)
      KT_E("timescale: DROP DATABASE '%s' failed: %s", dbName, PQerrorMessage(admin));
    PQclear(r);
    PQfinish(admin);
  }

  pthread_mutex_unlock(&poolCreateMutex);
  return TROE_OK;
}



// -----------------------------------------------------------------------------
//
// timescalePoolCloseAll -
//
void timescalePoolCloseAll(void)
{
  pthread_mutex_lock(&poolCreateMutex);

  if (tenant0.troePoolP != NULL)
  {
    poolFree((TimescalePool*) tenant0.troePoolP);
    tenant0.troePoolP = NULL;
  }

  for (Tenant* tP = tenantList; tP != NULL; tP = tP->next)
  {
    if (tP->troePoolP != NULL)
    {
      poolFree((TimescalePool*) tP->troePoolP);
      tP->troePoolP = NULL;
    }
  }

  pthread_mutex_unlock(&poolCreateMutex);
}

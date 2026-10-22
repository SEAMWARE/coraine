//
// FILE            timescaleWriter.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The TRoE writers of a tenant - see timescaleWriter.h.
//

#include <stdbool.h>                                      // bool
#include <stddef.h>                                       // NULL
#include <stdint.h>                                       // uint64_t, int64_t
#include <stdlib.h>                                       // calloc, free
#include <unistd.h>                                       // _exit
#include <time.h>                                         // clock_gettime
#include <pthread.h>                                      // pthread_*
#include <stdatomic.h>                                    // _Atomic, atomic_*
#include <libpq-fe.h>                                     // PG*

#include "corLog/corLog.h"                                // COR_E, COR_W, COR_I

#include "db/Tenant.h"                                    // Tenant, tenant0

#include "troe/TroeDriver.h"                              // TROE_OK, TROE_ERR, TroeHistoryStatus

#include "temporal/timescale/timescaleGlobals.h"          // timescaleWriters, timescaleDownExit
#include "temporal/timescale/timescalePool.h"             // TimescalePool, timescaleConnect, timescalePoolEnsure
#include "temporal/timescale/timescaleUnit.h"             // TsUnit, timescaleUnitsExec, tsUnitBytes, tsUnitRows
#include "temporal/timescale/timescaleWriter.h"           // Own interface



// -----------------------------------------------------------------------------
//
// The caps of a batch - the units of one transaction - and of the queue
//
// A batch takes at least one unit, however big. Past the queue cap a submitter waits for room: the
// writers are behind, and a queue without a bound would only turn that into memory. With the database
// down, nothing would make room: the unit is lost instead (timescaleHistoryLost).
//
#define BATCH_UNITS_MAX   512
#define BATCH_BYTES_MAX   (1 * 1024 * 1024)
#define QUEUE_BYTES_MAX   (8 * 1024 * 1024)



struct TsWriter;

// -----------------------------------------------------------------------------
//
// TsWriterThread - one writer: its thread, its connection, the first unit of the batch it is sending
//
typedef struct TsWriterThread
{
  struct TsWriter*  wP;
  pthread_t         tid;
  bool              started;
  PGconn*           conn;
  uint64_t          batchSeq;      // seq of the first unit of the batch in flight, 0 when idle
} TsWriterThread;



// -----------------------------------------------------------------------------
//
// TsWriter - the queue and the writers of a pool
//
typedef struct TsWriter
{
  struct TimescalePool*  poolP;
  pthread_mutex_t        mutex;
  pthread_cond_t         workCond;     // a unit is queued, or stop, or the database is back (CLOCK_MONOTONIC)
  pthread_cond_t         doneCond;     // a batch is done - for the barriers (a waiting submitter has its unit's)
  int                    barrierN;     // threads waiting in timescaleWriterBarrier
  pthread_cond_t         roomCond;     // the queue is below its cap again, or the database is down
  TsUnit*                head;
  TsUnit*                tail;
  int64_t                queuedBytes;
  int                    queuedN;      // units in the queue
  int                    busyN;        // writers sending a batch
  uint64_t               seq;          // seq of the last unit queued
  bool                   stop;
  int                    threadN;
  TsWriterThread*        threadV;

  bool                   down;         // the database cannot be reached: the units wait, one writer tries again
  bool                   probing;      // ... and it is trying
  int64_t                downSinceMs;  // monotonic
  int64_t                nextProbeMs;  // monotonic - the next try
  int                    backoffMs;    // 100 ms, twice as late each try, up to 5 s
  int64_t                lostAtDown;   // lostRows when it went down - what the outage lost, for the log
} TsWriter;



// -----------------------------------------------------------------------------
//
// The history over every tenant's writers - timescaleHistoryStatus
//
static _Atomic int      downN        = 0;        // pools that are down
static _Atomic int64_t  downSinceMs  = 0;        // monotonic ms the first of them went down - 0: none is
static _Atomic int64_t  heldBytes    = 0;        // queued, every pool
static _Atomic int64_t  lostRows     = 0;
static _Atomic int64_t  lostLoggedMs = 0;        // monotonic ms of the last loss logged



// -----------------------------------------------------------------------------
//
// monotonicMs -
//
static int64_t monotonicMs(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}



// -----------------------------------------------------------------------------
//
// condWaitMs - wait on a CLOCK_MONOTONIC condition for at most ms
//
static void condWaitMs(pthread_cond_t* condP, pthread_mutex_t* mutexP, int64_t ms)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  ts.tv_sec  += ms / 1000;
  ts.tv_nsec += (ms % 1000) * 1000000;
  if (ts.tv_nsec >= 1000000000)
  {
    ts.tv_sec  += 1;
    ts.tv_nsec -= 1000000000;
  }

  pthread_cond_timedwait(condP, mutexP, &ts);
}



// -----------------------------------------------------------------------------
//
// queueCount - bytes into (or, negative, out of) the counts of the queue only (mutex held)
//
// A batch a writer takes leaves the queue but is still held - it is held until it is recorded or
// lost: what the health report says is held must not drop to nothing while a try is in flight.
//
static void queueCount(TsWriter* wP, int64_t bytes, int n)
{
  wP->queuedBytes += bytes;
  wP->queuedN     += n;
}



// -----------------------------------------------------------------------------
//
// queueAdd - a unit's bytes into (or, negative, out of) the queue and what is held (mutex held)
//
static void queueAdd(TsWriter* wP, int64_t bytes, int n)
{
  queueCount(wP, bytes, n);
  atomic_fetch_add(&heldBytes, bytes);
}



// -----------------------------------------------------------------------------
//
// timescaleHistoryLost -
//
void timescaleHistoryLost(const char* dbName, int rowN, const char* why)
{
  int64_t lost = atomic_fetch_add(&lostRows, rowN) + rowN;

  if (timescaleDownExit > 0)
  {
    COR_E("timescale: TRoE: HISTORY LOST in db '%s': %s - %d row(s); --troeDownExit: the broker exits", dbName, why, rowN);
    _exit(1);
  }

  int64_t nowMs    = monotonicMs();
  int64_t loggedMs = atomic_load(&lostLoggedMs);

  if (((loggedMs == 0) || (nowMs - loggedMs >= 10000)) && (atomic_compare_exchange_strong(&lostLoggedMs, &loggedMs, nowMs) == true))
    COR_E("timescale: TRoE: HISTORY LOST in db '%s': %s - %lld row(s) of history lost since the start", dbName, why, (long long) lost);
}



// -----------------------------------------------------------------------------
//
// timescaleHistoryStatus -
//
void timescaleHistoryStatus(TroeHistoryStatus* sP)
{
  int64_t sinceMs = atomic_load(&downSinceMs);

  sP->recording = (atomic_load(&downN) == 0);
  sP->downForMs = ((sP->recording == false) && (sinceMs != 0)) ? monotonicMs() - sinceMs : 0;
  sP->heldBytes = atomic_load(&heldBytes);
  sP->lostRows  = atomic_load(&lostRows);
}



// -----------------------------------------------------------------------------
//
// writerConn - the writer's connection, connected or healed
//
static PGconn* writerConn(TsWriterThread* tP)
{
  if ((tP->conn != NULL) && (PQstatus(tP->conn) == CONNECTION_OK))
    return tP->conn;

  if (tP->conn != NULL)
  {
    PQreset(tP->conn);
    if (PQstatus(tP->conn) == CONNECTION_OK)
      return tP->conn;
    PQfinish(tP->conn);
  }

  tP->conn = timescaleConnect(tP->wP->poolP->dbName);
  return tP->conn;
}



// -----------------------------------------------------------------------------
//
// batchSend - one transaction for the batch; if it fails, one per unit, so one bad unit loses only itself
//
// Returns how many units have their outcome (rc): all of them, but for the database down - then the
// units from the one returned on were not recorded, and the database is the reason (not the rows).
//
static int batchSend(TsWriterThread* tP, TsUnit** unitV, int unitN)
{
  PGconn* conn = writerConn(tP);
  bool    down = false;

  if (conn == NULL)
    return 0;

  int rc = timescaleUnitsExec(conn, unitV, unitN, &down);

  if (down == true)
    return 0;

  if ((rc == TROE_OK) || (unitN == 1))
  {
    for (int ix = 0; ix < unitN; ix++)
      unitV[ix]->rc = rc;
    return unitN;
  }

  COR_W("timescale: TRoE batch of %d units failed - sending them one by one", unitN);
  for (int ix = 0; ix < unitN; ix++)
  {
    unitV[ix]->rc = timescaleUnitsExec(conn, &unitV[ix], 1, &down);

    if (down == true)
      return ix;
  }

  return unitN;
}



// -----------------------------------------------------------------------------
//
// unitDone - a unit has its outcome: to its submitter, or freed (mutex held)
//
static void unitDone(TsUnit* uP)
{
  if (uP->wait == true)
  {
    uP->done = true;                         // the submitter frees it
    pthread_cond_signal(&uP->doneCond);
  }
  else
    tsUnitFree(uP);
}



// -----------------------------------------------------------------------------
//
// waitersRelease - the database went down: a submitter waiting for its outcome is told it failed,
// and taken out of the queue (mutex held)
//
static void waitersRelease(TsWriter* wP)
{
  TsUnit* prevP = NULL;
  TsUnit* uP    = wP->head;

  while (uP != NULL)
  {
    TsUnit* nextP = uP->next;

    if (uP->wait == true)
    {
      if (prevP == NULL)
        wP->head = nextP;
      else
        prevP->next = nextP;
      if (wP->tail == uP)
        wP->tail = prevP;

      queueAdd(wP, -tsUnitBytes(uP), -1);
      uP->rc = TROE_ERR;
      unitDone(uP);
    }
    else
      prevP = uP;

    uP = nextP;
  }
}



// -----------------------------------------------------------------------------
//
// writerDown - a try found the database down (mutex held)
//
static void writerDown(TsWriter* wP)
{
  int64_t nowMs = monotonicMs();

  if (wP->down == false)
  {
    wP->down        = true;
    wP->downSinceMs = nowMs;
    wP->backoffMs   = 100;
    wP->lostAtDown  = atomic_load(&lostRows);

    if (atomic_fetch_add(&downN, 1) == 0)
      atomic_store(&downSinceMs, nowMs);

    COR_E("timescale: TRoE: the history database '%s' cannot be reached - THE HISTORY IS NOT BEING RECORDED. "
          "Held in memory up to %d MiB and retried; past that it is lost. The current state is served as before",
          wP->poolP->dbName, QUEUE_BYTES_MAX / (1024 * 1024));

    waitersRelease(wP);
    pthread_cond_broadcast(&wP->roomCond);   // a submitter waiting for room: none will come
    pthread_cond_broadcast(&wP->doneCond);   // a barrier: the queue will not empty
  }
  else if (wP->backoffMs < 5000)
    wP->backoffMs = (wP->backoffMs * 2 < 5000) ? wP->backoffMs * 2 : 5000;

  if (timescaleDownExit > 0)
  {
    int64_t exitMs = wP->downSinceMs + (int64_t) timescaleDownExit * 1000;

    if (nowMs >= exitMs)
    {
      COR_E("timescale: TRoE: the history database '%s' has been unreachable for %d s; --troeDownExit: the broker exits, "
            "%d request(s) of history held in memory not recorded", wP->poolP->dbName, timescaleDownExit, wP->queuedN);
      _exit(1);
    }

    if (nowMs + wP->backoffMs > exitMs)
    {
      wP->nextProbeMs = exitMs;
      return;
    }
  }

  wP->nextProbeMs = nowMs + wP->backoffMs;
}



// -----------------------------------------------------------------------------
//
// writerUp - a batch went in while the database was down: it is back (mutex held)
//
static void writerUp(TsWriter* wP)
{
  int64_t lost = atomic_load(&lostRows) - wP->lostAtDown;

  wP->down = false;
  if (atomic_fetch_sub(&downN, 1) == 1)
    atomic_store(&downSinceMs, 0);

  if (lost > 0)
    COR_E("timescale: TRoE: the history database '%s' is back after %lld s - recording again. %lld row(s) of history were LOST meanwhile",
          wP->poolP->dbName, (long long) ((monotonicMs() - wP->downSinceMs) / 1000), (long long) lost);
  else
    COR_W("timescale: TRoE: the history database '%s' is back after %lld s - recording again, nothing lost",
          wP->poolP->dbName, (long long) ((monotonicMs() - wP->downSinceMs) / 1000));

  pthread_cond_broadcast(&wP->workCond);
}



// -----------------------------------------------------------------------------
//
// workWait - until there is a batch to take, or stop (mutex held)
//
// The database down, only one writer tries again, and only once its time came; the others wait for it
// to be back.
//
static void workWait(TsWriter* wP)
{
  while (wP->stop == false)
  {
    if ((wP->head != NULL) && (wP->down == false))
      return;

    if ((wP->head != NULL) && (wP->probing == false))
    {
      int64_t waitMs = wP->nextProbeMs - monotonicMs();

      if (waitMs <= 0)
        return;

      condWaitMs(&wP->workCond, &wP->mutex, waitMs);
    }
    else
      pthread_cond_wait(&wP->workCond, &wP->mutex);
  }
}



// -----------------------------------------------------------------------------
//
// writerMain -
//
static void* writerMain(void* arg)
{
  TsWriterThread* tP = (TsWriterThread*) arg;
  TsWriter*       wP = tP->wP;
  TsUnit*         unitV[BATCH_UNITS_MAX];

  pthread_mutex_lock(&wP->mutex);

  while (true)
  {
    workWait(wP);

    if (wP->head == NULL)   // stopping, and nothing left to send
      break;

    // Take a batch off the queue
    int  unitN = 0;
    int  bytes = 0;
    bool probe = wP->down;

    if (probe == true)
      wP->probing = true;

    tP->batchSeq = wP->head->seq;

    while ((wP->head != NULL) && (unitN < BATCH_UNITS_MAX))
    {
      int uBytes = tsUnitBytes(wP->head);

      if ((unitN > 0) && (bytes + uBytes > BATCH_BYTES_MAX))
        break;

      unitV[unitN++] = wP->head;
      bytes         += uBytes;
      wP->head       = wP->head->next;
    }

    if (wP->head == NULL)
      wP->tail = NULL;
    else if (probe == false)
      pthread_cond_signal(&wP->workCond);    // more to do - for another writer

    queueCount(wP, -bytes, -unitN);
    wP->busyN += 1;
    pthread_cond_broadcast(&wP->roomCond);
    pthread_mutex_unlock(&wP->mutex);

    int doneN = batchSend(tP, unitV, unitN);

    pthread_mutex_lock(&wP->mutex);

    wP->busyN   -= 1;
    tP->batchSeq = 0;
    if (probe == true)
      wP->probing = false;

    //
    // Back only on the try's word: a batch taken before the database went down may still have gone in
    //
    if ((probe == true) && (doneN > 0) && (wP->down == true))
      writerUp(wP);

    int64_t doneBytes = 0;

    for (int ix = 0; ix < doneN; ix++)
    {
      doneBytes += tsUnitBytes(unitV[ix]);
      unitDone(unitV[ix]);
    }
    atomic_fetch_add(&heldBytes, -doneBytes);

    if ((doneN < unitN) && (wP->stop == false))
    {
      //
      // The database is down: the units not recorded back to the head of the queue, in their order -
      // their seq is still what the barriers count on
      //
      int64_t backBytes = 0;

      for (int ix = unitN - 1; ix >= doneN; ix--)
      {
        unitV[ix]->next = wP->head;
        wP->head        = unitV[ix];
        if (wP->tail == NULL)
          wP->tail = unitV[ix];
        backBytes += tsUnitBytes(unitV[ix]);
      }

      queueCount(wP, backBytes, unitN - doneN);   // still held - they never stopped being
      writerDown(wP);
    }
    else if (doneN < unitN)
    {
      //
      // Stopping, and the database down: what is held cannot be recorded - this batch's units and
      // every one still queued; one try, not one per batch
      //
      int     rowN      = 0;
      int64_t lostBytes = 0;

      for (int ix = doneN; ix < unitN; ix++)
      {
        rowN      += tsUnitRows(unitV[ix]);
        lostBytes += tsUnitBytes(unitV[ix]);
        unitV[ix]->rc = TROE_ERR;
        unitDone(unitV[ix]);
      }
      atomic_fetch_add(&heldBytes, -lostBytes);

      while (wP->head != NULL)
      {
        TsUnit* uP = wP->head;

        wP->head = uP->next;
        queueAdd(wP, -tsUnitBytes(uP), -1);
        rowN  += tsUnitRows(uP);
        uP->rc = TROE_ERR;
        unitDone(uP);
      }
      wP->tail = NULL;

      timescaleHistoryLost(wP->poolP->dbName, rowN, "the broker stops and the history database cannot be reached");
    }

    if (wP->barrierN > 0)
      pthread_cond_broadcast(&wP->doneCond);
  }

  pthread_mutex_unlock(&wP->mutex);
  return NULL;
}



// -----------------------------------------------------------------------------
//
// timescaleWriterStart -
//
int timescaleWriterStart(TimescalePool* poolP)
{
  if (timescaleWriters <= 0)
    return TROE_OK;

  TsWriter*       wP = (TsWriter*) calloc(1, sizeof(TsWriter));
  TsWriterThread* tV = (wP != NULL) ? (TsWriterThread*) calloc(timescaleWriters, sizeof(TsWriterThread)) : NULL;

  if (tV == NULL)
  {
    free(wP);
    COR_E("timescale: out of memory starting the TRoE writers of db '%s'", poolP->dbName);
    return TROE_ERR;
  }

  wP->poolP   = poolP;
  wP->threadN = timescaleWriters;
  wP->threadV = tV;
  pthread_mutex_init(&wP->mutex, NULL);

  pthread_condattr_t monotonic;
  pthread_condattr_init(&monotonic);
  pthread_condattr_setclock(&monotonic, CLOCK_MONOTONIC);
  pthread_cond_init(&wP->workCond, &monotonic);
  pthread_condattr_destroy(&monotonic);

  pthread_cond_init(&wP->doneCond, NULL);
  pthread_cond_init(&wP->roomCond, NULL);

  poolP->writerP = wP;

  for (int ix = 0; ix < wP->threadN; ix++)
  {
    tV[ix].wP = wP;

    if (pthread_create(&tV[ix].tid, NULL, writerMain, &tV[ix]) != 0)
    {
      COR_E("timescale: cannot start TRoE writer %d of db '%s'", ix, poolP->dbName);
      timescaleWriterStop(poolP);
      return TROE_ERR;
    }

    tV[ix].started = true;
  }

  return TROE_OK;
}



// -----------------------------------------------------------------------------
//
// timescaleWriterStop -
//
void timescaleWriterStop(TimescalePool* poolP)
{
  TsWriter* wP = (TsWriter*) poolP->writerP;

  if (wP == NULL)
    return;

  pthread_mutex_lock(&wP->mutex);
  wP->stop = true;
  pthread_cond_broadcast(&wP->workCond);
  pthread_mutex_unlock(&wP->mutex);

  for (int ix = 0; ix < wP->threadN; ix++)
  {
    if (wP->threadV[ix].started == true)
      pthread_join(wP->threadV[ix].tid, NULL);
    if (wP->threadV[ix].conn != NULL)
      PQfinish(wP->threadV[ix].conn);
  }

  if (wP->down == true)
  {
    if (atomic_fetch_sub(&downN, 1) == 1)
      atomic_store(&downSinceMs, 0);
  }

  poolP->writerP = NULL;

  pthread_cond_destroy(&wP->roomCond);
  pthread_cond_destroy(&wP->doneCond);
  pthread_cond_destroy(&wP->workCond);
  pthread_mutex_destroy(&wP->mutex);
  free(wP->threadV);
  free(wP);
}



// -----------------------------------------------------------------------------
//
// timescaleWriterSubmit -
//
bool timescaleWriterSubmit(Tenant* tenantP, TsUnit* uP, TsWait wait, int* rcP)
{
  if (tenantP == NULL)
    tenantP = &tenant0;

  if ((tenantP->troePoolP == NULL) && (timescalePoolEnsure(tenantP) != TROE_OK))
    return false;

  TimescalePool* poolP = (TimescalePool*) tenantP->troePoolP;
  TsWriter*      wP    = (TsWriter*) poolP->writerP;

  if (wP == NULL)
    return false;

  int bytes = tsUnitBytes(uP);

  pthread_mutex_lock(&wP->mutex);

  if (wP->stop == true)                      // shutting down - the caller writes it itself
  {
    pthread_mutex_unlock(&wP->mutex);
    return false;
  }

  //
  // The database down: an import is told at once
  //
  if ((wP->down == true) && (wait == TsWaitResult))
  {
    pthread_mutex_unlock(&wP->mutex);
    COR_W("timescale: TRoE: the history database '%s' cannot be reached - rows not recorded", poolP->dbName);
    tsUnitFree(uP);
    *rcP = TROE_ERR;
    return true;
  }

  while ((wP->queuedBytes > 0) && (wP->queuedBytes + bytes > QUEUE_BYTES_MAX) && (wP->stop == false) && (wP->down == false))
    pthread_cond_wait(&wP->roomCond, &wP->mutex);

  if ((wP->down == true) && (wP->queuedBytes > 0) && (wP->queuedBytes + bytes > QUEUE_BYTES_MAX))
  {
    int rowN = tsUnitRows(uP);

    pthread_mutex_unlock(&wP->mutex);
    tsUnitFree(uP);
    timescaleHistoryLost(poolP->dbName, rowN, "the database cannot be reached and the history held in memory is at its cap");
    *rcP = TROE_ERR;
    return true;
  }

  uP->next       = NULL;
  uP->wait = (wait == TsWaitResult);
  uP->done = false;
  uP->seq  = ++wP->seq;

  if (uP->wait == true)
    pthread_cond_init(&uP->doneCond, NULL);

  if (wP->tail == NULL)
    wP->head = uP;
  else
    wP->tail->next = uP;
  wP->tail = uP;
  queueAdd(wP, bytes, 1);

  //
  // Another writer only when none is sending, or the queue is past what a batch of a few requests
  // is: a writer that is busy takes what queued meanwhile when it is done, in one transaction. Woken
  // for every unit, under a light load each writer took one request and Postgres paid a transaction
  // for each.
  //
  if ((wP->busyN == 0) || (wP->queuedN >= 8))
    pthread_cond_signal(&wP->workCond);

  if (uP->wait == false)
  {
    pthread_mutex_unlock(&wP->mutex);
    *rcP = TROE_OK;
    return true;
  }

  while (uP->done == false)
    pthread_cond_wait(&uP->doneCond, &wP->mutex);

  pthread_cond_destroy(&uP->doneCond);

  *rcP = uP->rc;
  pthread_mutex_unlock(&wP->mutex);
  tsUnitFree(uP);
  return true;
}



// -----------------------------------------------------------------------------
//
// recordedSeq - every unit up to this seq is in the database (call with the mutex held)
//
// The queue's head and the batches in flight are what is not; the units before the oldest of them
// are done, whichever writer sent them.
//
static uint64_t recordedSeq(TsWriter* wP)
{
  uint64_t seq = (wP->head != NULL) ? wP->head->seq - 1 : wP->seq;

  for (int ix = 0; ix < wP->threadN; ix++)
  {
    uint64_t batchSeq = wP->threadV[ix].batchSeq;

    if ((batchSeq != 0) && (batchSeq - 1 < seq))
      seq = batchSeq - 1;
  }

  return seq;
}



// -----------------------------------------------------------------------------
//
// timescaleWriterBarrier -
//
// The database down, the queue will not empty: the caller goes on, and finds the database down too.
//
void timescaleWriterBarrier(TimescalePool* poolP)
{
  TsWriter* wP = (TsWriter*) poolP->writerP;

  if (wP == NULL)
    return;

  pthread_mutex_lock(&wP->mutex);

  uint64_t target = wP->seq;

  wP->barrierN += 1;
  while ((recordedSeq(wP) < target) && (wP->down == false))
    pthread_cond_wait(&wP->doneCond, &wP->mutex);
  wP->barrierN -= 1;

  pthread_mutex_unlock(&wP->mutex);
}

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
#include <pthread.h>                                      // pthread_*
#include <libpq-fe.h>                                     // PG*

#include "corLog/corLog.h"                                // COR_E, COR_W

#include "db/Tenant.h"                                    // Tenant, tenant0

#include "troe/TroeDriver.h"                              // TROE_OK, TROE_ERR

#include "temporal/timescale/timescaleGlobals.h"          // timescaleWriters
#include "temporal/timescale/timescalePool.h"             // TimescalePool, timescaleConnect, timescalePoolEnsure
#include "temporal/timescale/timescaleUnit.h"             // TsUnit, timescaleUnitsExec
#include "temporal/timescale/timescaleWriter.h"           // Own interface



// -----------------------------------------------------------------------------
//
// The caps of a batch - the units of one transaction - and of the queue
//
// A batch takes at least one unit, however big. Past the queue cap a submitter waits for room: the
// writers are behind, and a queue without a bound would only turn that into memory.
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
  pthread_cond_t         workCond;     // a unit is queued, or stop
  pthread_cond_t         doneCond;     // a batch is done - for the barriers (a waiting submitter has its unit's)
  int                    barrierN;     // threads waiting in timescaleWriterBarrier
  pthread_cond_t         roomCond;     // the queue is below its cap again
  TsUnit*                head;
  TsUnit*                tail;
  int64_t                queuedBytes;
  int                    queuedN;      // units in the queue
  int                    busyN;        // writers sending a batch
  uint64_t               seq;          // seq of the last unit queued
  bool                   stop;
  int                    threadN;
  TsWriterThread*        threadV;
} TsWriter;



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
static void batchSend(TsWriterThread* tP, TsUnit** unitV, int unitN)
{
  PGconn* conn = writerConn(tP);

  if (conn == NULL)
  {
    for (int ix = 0; ix < unitN; ix++)
      unitV[ix]->rc = TROE_ERR;
    COR_E("timescale: TRoE writer has no connection to db '%s' - %d unit(s) not recorded", tP->wP->poolP->dbName, unitN);
    return;
  }

  int rc = timescaleUnitsExec(conn, unitV, unitN);

  if ((rc == TROE_OK) || (unitN == 1))
  {
    for (int ix = 0; ix < unitN; ix++)
      unitV[ix]->rc = rc;
    return;
  }

  COR_W("timescale: TRoE batch of %d units failed - sending them one by one", unitN);
  for (int ix = 0; ix < unitN; ix++)
    unitV[ix]->rc = timescaleUnitsExec(conn, &unitV[ix], 1);
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
    while ((wP->head == NULL) && (wP->stop == false))
      pthread_cond_wait(&wP->workCond, &wP->mutex);

    if (wP->head == NULL)   // stopping, and nothing left to send
      break;

    // Take a batch off the queue
    int unitN = 0;
    int bytes = 0;

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
    else
      pthread_cond_signal(&wP->workCond);    // more to do - for another writer

    wP->queuedBytes -= bytes;
    wP->queuedN     -= unitN;
    wP->busyN       += 1;
    pthread_cond_broadcast(&wP->roomCond);
    pthread_mutex_unlock(&wP->mutex);

    batchSend(tP, unitV, unitN);

    pthread_mutex_lock(&wP->mutex);

    wP->busyN   -= 1;
    tP->batchSeq = 0;
    for (int ix = 0; ix < unitN; ix++)
    {
      if (unitV[ix]->wait == true)
      {
        unitV[ix]->done = true;              // the submitter frees it
        pthread_cond_signal(&unitV[ix]->doneCond);
      }
      else
        tsUnitFree(unitV[ix]);
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
  pthread_cond_init(&wP->workCond, NULL);
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
bool timescaleWriterSubmit(Tenant* tenantP, TsUnit* uP, bool wait, int* rcP)
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

  uP->next = NULL;
  uP->wait = wait;
  uP->done = false;

  if (wait == true)
    pthread_cond_init(&uP->doneCond, NULL);

  pthread_mutex_lock(&wP->mutex);

  if (wP->stop == true)                      // shutting down - the caller writes it itself
  {
    pthread_mutex_unlock(&wP->mutex);
    if (wait == true)
      pthread_cond_destroy(&uP->doneCond);
    return false;
  }

  while ((wP->queuedBytes > 0) && (wP->queuedBytes + bytes > QUEUE_BYTES_MAX) && (wP->stop == false))
    pthread_cond_wait(&wP->roomCond, &wP->mutex);

  uP->seq = ++wP->seq;

  if (wP->tail == NULL)
    wP->head = uP;
  else
    wP->tail->next = uP;
  wP->tail         = uP;
  wP->queuedBytes += bytes;
  wP->queuedN     += 1;

  //
  // Another writer only when none is sending, or the queue is past what a batch of a few requests
  // is: a writer that is busy takes what queued meanwhile when it is done, in one transaction. Woken
  // for every unit, under a light load each writer took one request and Postgres paid a transaction
  // for each.
  //
  if ((wP->busyN == 0) || (wP->queuedN >= 8))
    pthread_cond_signal(&wP->workCond);

  if (wait == true)
  {
    while (uP->done == false)
      pthread_cond_wait(&uP->doneCond, &wP->mutex);

    *rcP = uP->rc;
    pthread_mutex_unlock(&wP->mutex);
    pthread_cond_destroy(&uP->doneCond);
    tsUnitFree(uP);
    return true;
  }

  pthread_mutex_unlock(&wP->mutex);
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
void timescaleWriterBarrier(TimescalePool* poolP)
{
  TsWriter* wP = (TsWriter*) poolP->writerP;

  if (wP == NULL)
    return;

  pthread_mutex_lock(&wP->mutex);

  uint64_t target = wP->seq;

  wP->barrierN += 1;
  while (recordedSeq(wP) < target)
    pthread_cond_wait(&wP->doneCond, &wP->mutex);
  wP->barrierN -= 1;

  pthread_mutex_unlock(&wP->mutex);
}

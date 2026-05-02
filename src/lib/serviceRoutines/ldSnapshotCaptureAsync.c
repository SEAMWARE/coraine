//
// FILE            ldSnapshotCaptureAsync.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Background-thread Snapshot capture — see header.
//
#include <pthread.h>                                     // pthread_create, pthread_detach
#include <stdbool.h>                                     // bool
#include <stdlib.h>                                      // calloc, free
#include <string.h>                                      // memset
#include <time.h>                                        // clock_gettime

#include "ktrace/kTrace.h"                               // KT_E
#include "kalloc/kaBufferInit.h"                         // kaBufferInit
#include "kalloc/kaBufferReset.h"                        // kaBufferReset
#include "kjson/KjNode.h"                                // KjNode
#include "kjson/kjBufferCreate.h"                        // kjBufferCreate
#include "kjson/kjLookup.h"                              // kjLookup
#include "kjson/kjBuilder.h"                             // kjObject, kjString, kjChildAdd
#include "kjson/kjClone.h"                               // kjClone

#include "swRest/SwRestState.h"                          // swRest (__thread)
#include "swNgsild/SwNgsild.h"                           // swNgsild (__thread)
#include "swNgsild/ldSnapshotNotify.h"                   // ldSnapshotNotify

#include "db/DbDriver.h"                                 // db, DB_OK
#include "db/Tenant.h"                                   // Tenant

#include "serviceRoutines/ldSnapshotExec.h"              // ldSnapshotExecQueries
#include "serviceRoutines/ldSnapshotExecTemporal.h"      // ldSnapshotExecTemporalQueries
#include "serviceRoutines/ldSnapshotCaptureAsync.h"      // Own interface


// -----------------------------------------------------------------------------
//
// SnapshotCaptureCtx - per-thread capture context.
//
// Allocated on heap by the spawner, freed by the worker on exit.
//
typedef struct SnapshotCaptureCtx
{
  LdSnapshotCache*      cacheP;
  LdSnapshotCacheItem*  itemP;
  Tenant*               tenantP;
  bool                  splitEntitiesSet;
  bool                  splitEntitiesVal;
} SnapshotCaptureCtx;



//
// snapshotWorkerThread - worker entry point.
//
// Initializes the per-thread swRest / swNgsild state, runs the capture,
// persists the final state via db.snapshotUpdate, fires the notification,
// and exits.
//
static void* snapshotWorkerThread(void* arg)
{
  SnapshotCaptureCtx* ctx = (SnapshotCaptureCtx*) arg;
  if (ctx == NULL) return NULL;

  // Per-thread swRest init — minimal. We're not handling an MHD request,
  // so we skip swRestStateInit and set up only what the DB / notify code
  // touches: kalloc, kjsonP, requestStartTime.
  memset(&swRest, 0, sizeof(swRest));
  kaBufferInit(&swRest.kalloc, swRest.kallocBuffer, sizeof(swRest.kallocBuffer),
               256 * 1024, NULL, "snap-async");
  swRest.kjsonP = kjBufferCreate(&swRest.kjson, &swRest.kalloc);

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  swRest.requestStartTime = (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;

  // Per-thread swNgsild init. The worker only uses tenantP and the
  // splitEntities* fields (via ldSnapshotExec's runOneQuery).
  memset(&swNgsild, 0, sizeof(swNgsild));
  swNgsild.tenantP          = ctx->tenantP;
  swNgsild.splitEntitiesSet = ctx->splitEntitiesSet;
  swNgsild.splitEntitiesVal = ctx->splitEntitiesVal;

  ldSnapshotExecQueries(ctx->cacheP, ctx->itemP, ctx->tenantP);
  ldSnapshotExecTemporalQueries(ctx->cacheP, ctx->itemP, ctx->tenantP);

  // Persist the now-updated metadata (status + both detail arrays)
  // via JSON Merge Patch.
  if (db.snapshotUpdate != NULL && ctx->itemP->tree != NULL)
  {
    KjNode* fragment = kjObject(swRest.kjsonP, NULL);
    KjNode* sP       = kjLookup(ctx->itemP->tree, "snapshotStatus");
    if (sP != NULL && sP->type == KjString)
      kjChildAdd(fragment, kjString(swRest.kjsonP, "snapshotStatus", sP->value.s));
    KjNode* dP = kjLookup(ctx->itemP->tree, "snapshotQueriesDetails");
    if (dP != NULL)
      kjChildAdd(fragment, kjClone(swRest.kjsonP, dP));
    KjNode* tdP = kjLookup(ctx->itemP->tree, "snapshotTemporalQueriesDetails");
    if (tdP != NULL)
      kjChildAdd(fragment, kjClone(swRest.kjsonP, tdP));
    if (fragment->value.firstChildP != NULL)
      db.snapshotUpdate(ctx->tenantP, ctx->itemP->id, fragment);
  }

  // § 5.16.6 — fire SnapshotNotification on capture completion.
  ldSnapshotNotify(ctx->itemP, false);

  // Free per-thread resources. kalloc inline buffer is on the thread's
  // stack — wait, no, it's __thread, so it persists with the thread.
  // The thread is about to exit so nothing to free explicitly; the OS
  // reclaims thread-locals. kaBuffer's malloc-overflow blocks need to be
  // freed via kaBufferReset.
  kaBufferReset(&swRest.kalloc, true);

  free(ctx);
  return NULL;
}



void ldSnapshotCaptureAsync(LdSnapshotCache*     cacheP,
                             LdSnapshotCacheItem* itemP,
                             Tenant*              tenantP,
                             bool                 splitEntitiesSet,
                             bool                 splitEntitiesVal)
{
  SnapshotCaptureCtx* ctx = (SnapshotCaptureCtx*) calloc(1, sizeof(*ctx));
  if (ctx == NULL)
  {
    KT_E("snapshotCaptureAsync: ctx alloc failed; falling back to inline exec");
    ldSnapshotExecQueries(cacheP, itemP, tenantP);
    ldSnapshotNotify(itemP, false);
    return;
  }

  ctx->cacheP            = cacheP;
  ctx->itemP             = itemP;
  ctx->tenantP           = tenantP;
  ctx->splitEntitiesSet  = splitEntitiesSet;
  ctx->splitEntitiesVal  = splitEntitiesVal;

  pthread_t tid;
  if (pthread_create(&tid, NULL, snapshotWorkerThread, ctx) != 0)
  {
    KT_E("snapshotCaptureAsync: pthread_create failed; falling back to inline exec");
    free(ctx);
    ldSnapshotExecQueries(cacheP, itemP, tenantP);
    ldSnapshotNotify(itemP, false);
    return;
  }
  pthread_detach(tid);
}

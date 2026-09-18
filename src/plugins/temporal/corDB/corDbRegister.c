//
// FILE            corDbRegister.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// "corDB" troe plugin — in-memory ring buffer for dev/test.
//
// Captures the most recent N events. The ring is global to the broker
// process (shared across worker threads); access is mutex-guarded so a
// concurrent dump-and-write doesn't tear.
//
// The admin route /admin/troe/dump invokes troe.dumpInfo to render the
// current ring contents as a JSON array. Functests use it to assert
// that a write produced the expected TRoE events.
//
#define PLUGIN_VERSION "0.1.0"

#define RING_SIZE 256

#include <stddef.h>                                       // NULL
#include <string.h>                                       // memset, strcpy
#include <pthread.h>                                      // pthread_mutex_*
#include <stdlib.h>                                       // strtol — unused but safe
#include <stdio.h>                                        // snprintf

#include "kalloc/kalloc.h"                                // kaBufferInit
#include "kalloc/kaAlloc.h"                              // kaAlloc
#include "kalloc/kaStrdup.h"                              // kaStrdup
#include "kjson/kjson.h"                                  // Kjson
#include "kjson/kjBufferCreate.h"                         // kjBufferCreate
#include "kjson/kjBuilder.h"                              // kjArray, kjObject, kjString, kjInteger, kjChildAdd
#include "kjson/KjNode.h"                                 // KjNode

#include "troe/TroeDriver.h"                              // TroeDriver



// -----------------------------------------------------------------------------
//
// Captured event - simplified copy-out of TroeEvent.
//
// We don't keep the original TroeEvent because its allocator is
// per-request and gets reset. Strings get kaStrdup'd and the snapshot
// kjClone'd onto the plugin's own allocator.
//
typedef struct
{
  bool         used;
  TroeOp       op;
  uint64_t     modifiedAtNs;
  char*        entityId;        // strdup
  char*        entityType;      // strdup
  char*        attrName;        // strdup, NULL for entity-level
  char*        datasetId;       // strdup, may be NULL
  // Snapshots aren't kept here in the first cut — the plugin captures the
  // metadata that's cheap and observable. Adding cloned snapshots is a
  // follow-up if functests need them.
} CapturedEvent;



// -----------------------------------------------------------------------------
//
// Ring state — protected by corDbTroeMutex.
//
static CapturedEvent     ring[RING_SIZE];
static int               ringHead  = 0;     // next slot to write
static int               ringCount = 0;     // number of valid entries (0..RING_SIZE)
static pthread_mutex_t   corDbTroeMutex = PTHREAD_MUTEX_INITIALIZER;
static KAlloc            corDbTroeAlloc;
static char              corDbTroeAllocBuf[64 * 1024];



// -----------------------------------------------------------------------------
//
// corDbTroeInit / corDbTroeClose -
//
static int corDbTroeInit(void)
{
  kaBufferInit(&corDbTroeAlloc, corDbTroeAllocBuf, sizeof(corDbTroeAllocBuf), 16 * 1024, NULL, "troeCorDb");
  return TROE_OK;
}

static void corDbTroeClose(void) { }



// -----------------------------------------------------------------------------
//
// captureEvent - copy the event into the ring under mutex.
//
static void captureEvent(const TroeEvent* evP)
{
  pthread_mutex_lock(&corDbTroeMutex);

  CapturedEvent* slot = &ring[ringHead];
  memset(slot, 0, sizeof(*slot));

  slot->used         = true;
  slot->op           = evP->op;
  slot->modifiedAtNs = evP->modifiedAtNs;
  slot->entityId     = (evP->entityId != NULL)   ? kaStrdup(&corDbTroeAlloc, evP->entityId)   : NULL;
  slot->entityType   = (evP->entityType != NULL) ? kaStrdup(&corDbTroeAlloc, evP->entityType) : NULL;
  slot->attrName     = (evP->attrName != NULL)   ? kaStrdup(&corDbTroeAlloc, evP->attrName)   : NULL;
  slot->datasetId    = (evP->datasetId != NULL)  ? kaStrdup(&corDbTroeAlloc, evP->datasetId)  : NULL;

  ringHead = (ringHead + 1) % RING_SIZE;
  if (ringCount < RING_SIZE) ringCount++;

  pthread_mutex_unlock(&corDbTroeMutex);
}



static int corDbTroeEntityEvent(const TroeEvent* evP) { captureEvent(evP); return TROE_OK; }
static int corDbTroeAttrEvent  (const TroeEvent* evP) { captureEvent(evP); return TROE_OK; }



// -----------------------------------------------------------------------------
//
// opName -
//
static const char* opName(TroeOp op)
{
  switch (op)
  {
    case TroeOpEntityCreated:  return "entityCreated";
    case TroeOpEntityReplaced: return "entityReplaced";
    case TroeOpEntityDeleted:  return "entityDeleted";
    case TroeOpAttrCreated:    return "attrCreated";
    case TroeOpAttrModified:   return "attrModified";
    case TroeOpAttrReplaced:   return "attrReplaced";
    case TroeOpAttrDeleted:    return "attrDeleted";
  }
  return "unknown";
}



// -----------------------------------------------------------------------------
//
// corDbTroeDumpInfo - render the ring contents as a JSON array under root["events"].
//
// Walks the ring oldest→newest. Allocates onto allocP (the caller's
// request arena), so nothing in the produced tree references the
// plugin's own buffer.
//
static void corDbTroeDumpInfo(KAlloc* allocP, KjNode* root)
{
  Kjson  kjsonLocal;
  Kjson* kjsonP = kjBufferCreate(&kjsonLocal, allocP);

  KjNode* arr = kjArray(kjsonP, "events");

  pthread_mutex_lock(&corDbTroeMutex);

  // Oldest entry is at (ringHead - ringCount), wrapping. ringCount==RING_SIZE
  // when full → start at ringHead. Otherwise start at 0.
  int start = (ringCount == RING_SIZE) ? ringHead : 0;

  for (int i = 0; i < ringCount; i++)
  {
    int ix = (start + i) % RING_SIZE;
    CapturedEvent* e = &ring[ix];
    if (!e->used) continue;

    KjNode* obj = kjObject(kjsonP, NULL);
    kjChildAdd(obj, kjString(kjsonP, "op", opName(e->op)));
    kjChildAdd(obj, kjInteger(kjsonP, "modifiedAtNs", (long long) e->modifiedAtNs));
    if (e->entityId   != NULL) kjChildAdd(obj, kjString(kjsonP, "entityId",   e->entityId));
    if (e->entityType != NULL) kjChildAdd(obj, kjString(kjsonP, "entityType", e->entityType));
    if (e->attrName   != NULL) kjChildAdd(obj, kjString(kjsonP, "attrName",   e->attrName));
    if (e->datasetId  != NULL) kjChildAdd(obj, kjString(kjsonP, "datasetId",  e->datasetId));

    kjChildAdd(arr, obj);
  }

  pthread_mutex_unlock(&corDbTroeMutex);

  kjChildAdd(root, arr);
}



// -----------------------------------------------------------------------------
//
// troeRegister -
//
void troeRegister(TroeDriver* driverP)
{
  driverP->alias        = "corDB";
  driverP->version      = PLUGIN_VERSION;
  driverP->args         = NULL;
  driverP->init         = corDbTroeInit;
  driverP->close        = corDbTroeClose;
  driverP->tenantSetup  = NULL;
  driverP->migrate      = NULL;
  driverP->entityEvent  = corDbTroeEntityEvent;
  driverP->attrEvent    = corDbTroeAttrEvent;
  driverP->eventList    = NULL;
  driverP->entityTemporalQuery    = NULL;
  driverP->entityTemporalRetrieve = NULL;
  driverP->versionInfo  = NULL;
  driverP->dumpInfo     = corDbTroeDumpInfo;
}

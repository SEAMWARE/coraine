//
// FILE            bridgeServiceSync.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <errno.h>                                    // ETIMEDOUT
#include <pthread.h>                                  // pthread_mutex_*, pthread_cond_*
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // uint64_t, int64_t
#include <stdlib.h>                                   // free
#include <string.h>                                   // strcmp, strdup
#include <time.h>                                     // clock_gettime

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/kjRender.h"                           // kjFastRender
#include "kjson/kjBuilder.h"                          // kjChildAdd, kjChildRemove
#include "ktrace/kTrace.h"                            // KT_T, KT_W
#include "corRest/corRest.h"                          // corRest
#include "corNgsild/corNgsild.h"                      // ldError, LD_ERROR_*
#include "corNgsild/ldIsEntityKeyword.h"              // ldIsNotAttributeName
#include "corBridge/BridgeDriver.h"                   // BridgeDriver, bridges, bridgeCount
#include "corBridge/corBridge.h"                      // corBridgeKindName
#include "bridge/Channel.h"                           // Channel
#include "bridge/channelCache.h"                      // channelLookupByTarget, channelCount
#include "bridge/bridgeSampleIn.h"                    // bridgeSampleQualifiedIn, bridgeReplySubAttr
#include "bridge/bridgeServiceSync.h"                 // Own interface
#include "coraineTraceLevels.h"                       // KtBridge



// -----------------------------------------------------------------------------
//
// bridgeSyncDefault / bridgeSyncTimeoutMs - --ddsSync, --ddsSyncTimeout
//
bool bridgeSyncDefault   = false;
int  bridgeSyncTimeoutMs = 5000;



// -----------------------------------------------------------------------------
//
// SYNC_OUT_MAX - the largest request payload a synchronous invocation renders
//
// As bridgeAttrOut's buffer, and for the same reason: a service request is the
// value of one attribute.
//
#define SYNC_OUT_MAX  (64 * 1024)



// -----------------------------------------------------------------------------
//
// ABANDONED_KEEP_MS - how long the token of a request that gave up is remembered
//
// Its late reply must be recognised as late and dropped, rather than taken for
// an ordinary asynchronous one and written - the request answered 504 and wrote
// nothing, and a reply turning up in the attribute a minute later would say
// otherwise. A reply later than this is so late that the service has, to every
// purpose, not answered; forgetting the token bounds what a service that never
// answers can cost.
//
#define ABANDONED_KEEP_MS  (60 * 1000)



// -----------------------------------------------------------------------------
//
// SyncWaiter - one invocation that a request is waiting for
//
// ⭐ ON THE HEAP, NOT ON THE WAITING THREAD'S STACK. A request that times out
// returns, and its stack goes with it - but the reply may still come, on a
// transport thread, looking for this very entry. So the entry outlives the wait
// when it has to: it is freed by whichever side is LAST to touch it, the
// waiter on success, the reply (or the sweep) after a timeout.
//
typedef enum SyncState
{
  SyncWaiting   = 0,                                  // invoked, no reply yet
  SyncAnswered  = 1,                                  // the reply is here, for the waiter to take
  SyncAbandoned = 2                                   // the waiter gave up; a reply is to be dropped
} SyncState;

typedef struct SyncWaiter
{
  uint64_t            token;
  SyncState           state;
  pthread_cond_t      cond;

  // The reply - copied, since it arrives on a transport thread whose buffers
  // are gone the moment replyIn returns
  char*               subAttrName;
  char*               json;
  int64_t             publishTime;

  int64_t             abandonedAtMs;
  struct SyncWaiter*  next;
} SyncWaiter;

static pthread_mutex_t  syncMutex   = PTHREAD_MUTEX_INITIALIZER;
static SyncWaiter*      waiters     = NULL;
static uint64_t         nextToken   = 1;



// -----------------------------------------------------------------------------
//
// nowMs - the monotonic clock, as the waits use it
//
static int64_t nowMs(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}



// -----------------------------------------------------------------------------
//
// waiterFree - caller holds syncMutex and has unlinked it
//
static void waiterFree(SyncWaiter* wP)
{
  pthread_cond_destroy(&wP->cond);
  free(wP->subAttrName);
  free(wP->json);
  free(wP);
}



// -----------------------------------------------------------------------------
//
// waiterUnlink - caller holds syncMutex
//
static void waiterUnlink(SyncWaiter* wP)
{
  SyncWaiter** pP = &waiters;

  while ((*pP != NULL) && (*pP != wP))
    pP = &(*pP)->next;

  if (*pP != NULL)
    *pP = wP->next;
}



// -----------------------------------------------------------------------------
//
// waiterSweep - forget the tokens of requests that gave up long ago
//
// Caller holds syncMutex.
//
static void waiterSweep(void)
{
  int64_t      now = nowMs();
  SyncWaiter** pP  = &waiters;

  while (*pP != NULL)
  {
    SyncWaiter* wP = *pP;

    if ((wP->state == SyncAbandoned) && (now - wP->abandonedAtMs > ABANDONED_KEEP_MS))
    {
      *pP = wP->next;
      waiterFree(wP);
    }
    else
      pP = &wP->next;
  }
}



// -----------------------------------------------------------------------------
//
// waiterCreate - register a new invocation, BEFORE it is sent
//
// ⭐ BEFORE, because the reply can arrive before the invocation returns - a
// transport may answer on another thread at once, or inline. An entry made
// afterwards would miss it, and the request would time out on a reply that had
// already come.
//
static SyncWaiter* waiterCreate(void)
{
  SyncWaiter* wP = (SyncWaiter*) calloc(1, sizeof(SyncWaiter));

  if (wP == NULL)
    return NULL;

  pthread_condattr_t attr;

  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&wP->cond, &attr);
  pthread_condattr_destroy(&attr);

  wP->state = SyncWaiting;

  pthread_mutex_lock(&syncMutex);
  waiterSweep();

  wP->token = nextToken++;
  if (nextToken == 0)                                 // 0 means "untracked" on the seam - never hand it out
    nextToken = 1;

  wP->next = waiters;
  waiters  = wP;
  pthread_mutex_unlock(&syncMutex);

  return wP;
}



// -----------------------------------------------------------------------------
//
// bridgeSyncRequested -
//
bool bridgeSyncRequested(bool* syncP)
{
  *syncP = bridgeSyncDefault;

  for (int ix = 0; ix < corRest.in.uriParamCount; ix++)
  {
    if (strcmp(corRest.in.uriParamV[ix].key, "ddsSync") != 0)
      continue;

    const char* value = corRest.in.uriParamV[ix].value;

    if ((value != NULL) && (strcmp(value, "true") == 0))
      *syncP = true;
    else if ((value != NULL) && (strcmp(value, "false") == 0))
      *syncP = false;
    else
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid request",
              "'ddsSync' must be 'true' or 'false'");
      return false;
    }
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// bridgeSyncDoneHas -
//
bool bridgeSyncDoneHas(const BridgeSyncDone* doneP, const Channel* channelP)
{
  if (doneP == NULL)
    return false;

  for (int ix = 0; ix < doneP->count; ix++)
  {
    if (doneP->channelV[ix] == channelP)
      return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// driverFor - the loaded bridge that carries a Channel
//
static BridgeDriver* driverFor(const Channel* channelP)
{
  for (int ix = 0; ix < bridgeCount; ix++)
  {
    if ((bridges[ix].alias != NULL) && (strcmp(bridges[ix].alias, channelP->bridgeName) == 0))
      return &bridges[ix];
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// syncInvoke - invoke one service and wait for its reply
//
// @return true with *waiterPP holding the answered entry, which the caller
//         frees; false with the error set.
//
static bool syncInvoke(const char* entityId, const char* attrName, Channel* channelP, const char* json, SyncWaiter** waiterPP)
{
  BridgeDriver* driverP = driverFor(channelP);

  if ((driverP == NULL) || (driverP->serviceInvokeTracked == NULL))
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Operation Not Supported",
            "bridge '%s' cannot wait for the reply of service '%s' - ddsSync is not available for it",
            channelP->bridgeName, channelP->endpoint);
    return false;
  }

  SyncWaiter* wP = waiterCreate();

  if (wP == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "out of memory");
    return false;
  }

  int r = driverP->serviceInvokeTracked(channelP->endpoint, json, wP->token);

  if (r != BRIDGE_OK)
  {
    pthread_mutex_lock(&syncMutex);
    waiterUnlink(wP);
    waiterFree(wP);
    pthread_mutex_unlock(&syncMutex);

    if (r == BRIDGE_BAD_INPUT)
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid request",
              "the value of '%s' does not fit the request of service '%s'", attrName, channelP->endpoint);
    else
      ldError(503, LD_ERROR_INTERNAL_ERROR, "Service Unavailable",
              "service '%s' on bridge '%s' could not be reached", channelP->endpoint, channelP->bridgeName);

    return false;
  }

  KT_T(KtBridge, "%s/%s asks service '%s' on bridge '%s', and waits (token %llu)",
       entityId, attrName, channelP->endpoint, channelP->bridgeName, (unsigned long long) wP->token);

  //
  // The deadline, on the same clock the condition variable was told to use.
  //
  struct timespec deadline;
  int64_t         dueMs = nowMs() + bridgeSyncTimeoutMs;

  deadline.tv_sec  = dueMs / 1000;
  deadline.tv_nsec = (dueMs % 1000) * 1000000;

  pthread_mutex_lock(&syncMutex);
  while (wP->state == SyncWaiting)
  {
    if (pthread_cond_timedwait(&wP->cond, &syncMutex, &deadline) == ETIMEDOUT)
      break;
  }

  if (wP->state != SyncAnswered)
  {
    //
    // Left in the list, marked: the reply may yet come, and has to be known for
    // what it is when it does. See ABANDONED_KEEP_MS.
    //
    wP->state         = SyncAbandoned;
    wP->abandonedAtMs = nowMs();
    pthread_mutex_unlock(&syncMutex);

    ldError(504, LD_ERROR_INTERNAL_ERROR, "Service Timeout",
            "service '%s' on bridge '%s' did not answer within %d ms",
            channelP->endpoint, channelP->bridgeName, bridgeSyncTimeoutMs);
    return false;
  }

  waiterUnlink(wP);
  pthread_mutex_unlock(&syncMutex);

  *waiterPP = wP;
  return true;
}



// -----------------------------------------------------------------------------
//
// bridgeSyncFragment -
//
bool bridgeSyncFragment(Tenant* tenantP, const char* entityId, KjNode* fragmentP, BridgeSyncDone* doneP)
{
  doneP->count = 0;

  bool sync;

  if (bridgeSyncRequested(&sync) == false)
    return false;

  if (sync == false)
    return true;

  if ((bridgeCount == 0) || (channelCount() == 0) || (entityId == NULL) || (fragmentP == NULL))
    return true;

  for (KjNode* attrP = fragmentP->value.firstChildP; attrP != NULL; attrP = attrP->next)
  {
    if (ldIsNotAttributeName(attrP->name) == true)
      continue;

    Channel* channelP = channelLookupByTarget(tenantP, entityId, attrP->name);

    if ((channelP == NULL) || (channelP->kind != BridgeChannelService))
      continue;

    if ((channelP->direction == BridgeDirectionIn) || (channelP->status != ChannelStatusAvailable))
      continue;

    //
    // The default instance only. A service answers the one request it was
    // sent, and an attribute written as several datasetId instances at once is
    // not one request - those go the asynchronous way, after the write.
    //
    KjNode* instanceP = kjLookup(attrP, "@none");
    KjNode* valueP    = (instanceP != NULL) ? kjLookup(instanceP, "value") : NULL;

    if (valueP == NULL)
      continue;

    if (doneP->count >= BRIDGE_SYNC_MAX)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid request",
              "a synchronous request may invoke at most %d services", BRIDGE_SYNC_MAX);
      return false;
    }

    static __thread char buf[SYNC_OUT_MAX];
    char*   savedName = valueP->name;
    KjNode* savedNext = valueP->next;

    valueP->name = NULL;
    valueP->next = NULL;
    kjFastRender(valueP, buf);
    valueP->name = savedName;
    valueP->next = savedNext;

    SyncWaiter* wP = NULL;

    if (syncInvoke(entityId, attrP->name, channelP, buf, &wP) == false)
      return false;

    KjNode* replyP = bridgeReplySubAttr(attrP->name, wP->subAttrName, wP->json, wP->publishTime);

    if (replyP == NULL)
    {
      ldError(502, LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
              "service '%s' on bridge '%s' answered with something that is not JSON",
              channelP->endpoint, channelP->bridgeName);
      waiterFree(wP);
      return false;
    }

    //
    // Replacing a previous reply the fragment might carry - the request's own
    // payload naming the sub-attribute the answer goes in is the client's
    // business, but this request's answer is what goes in it.
    //
    KjNode* oldP = kjLookup(instanceP, replyP->name);

    if (oldP != NULL)
      kjChildRemove(instanceP, oldP);

    kjChildAdd(instanceP, replyP);

    doneP->channelV[doneP->count++] = channelP;
    waiterFree(wP);
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// bridgeReplyIn -
//
int bridgeReplyIn(const char* bridgeName,
                  const char* endpoint,
                  uint64_t    token,
                  const char* datasetId,
                  const char* subAttrName,
                  const char* json,
                  int64_t     publishTime)
{
  if (token != 0)
  {
    pthread_mutex_lock(&syncMutex);

    SyncWaiter* wP = waiters;

    while ((wP != NULL) && (wP->token != token))
      wP = wP->next;

    if ((wP != NULL) && (wP->state == SyncWaiting))
    {
      wP->subAttrName = (subAttrName != NULL) ? strdup(subAttrName) : NULL;
      wP->json        = (json        != NULL) ? strdup(json)        : NULL;
      wP->publishTime = publishTime;
      wP->state       = SyncAnswered;

      pthread_cond_signal(&wP->cond);
      pthread_mutex_unlock(&syncMutex);
      return BRIDGE_OK;
    }

    if ((wP != NULL) && (wP->state == SyncAbandoned))
    {
      waiterUnlink(wP);
      waiterFree(wP);
      pthread_mutex_unlock(&syncMutex);

      KT_W("bridge '%s': service '%s' answered after its request had given up - the reply is dropped",
           (bridgeName != NULL) ? bridgeName : "?", (endpoint != NULL) ? endpoint : "?");
      return BRIDGE_OK;
    }

    pthread_mutex_unlock(&syncMutex);
  }

  //
  // Nobody is waiting for it - an ordinary reply, to an ordinary invocation.
  //
  return bridgeSampleQualifiedIn(bridgeName, endpoint, datasetId, subAttrName, json, publishTime);
}

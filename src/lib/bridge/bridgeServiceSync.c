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
#include "bridge/bridgeGoal.h"                        // bridgeGoalSend
#include "bridge/bridgeSampleIn.h"                    // bridgeSampleQualifiedIn, bridgeReplySubAttr
#include "bridge/bridgeServiceSync.h"                 // Own interface
#include "coraineTraceLevels.h"                       // KtBridge



// -----------------------------------------------------------------------------
//
// bridgeSyncDefault / bridgeSyncTimeoutMs - --ddsSync, --ddsSyncTimeout
//
bool bridgeSyncDefault   = false;
//
// The defaults, from a real ROS 2 service over DDS on one machine (2026-09-24):
// a waited-for call answered in 2.0 ms at the median and 5.6 ms at worst with 8
// in parallel. 200 ms is some 40 times that - room for a network hop and a
// service that works a little - and a request that waits longer is answered
// 202 anyway, its reply landing when it comes. 8 waiters is a quarter of the
// default 32 workers: at 3 ms a call that is some 2500 waited-for requests a
// second, and a DDS side that answers nothing can tie up 8 workers, never 32.
//
int  bridgeSyncTimeoutMs = 200;
int  bridgeSyncWaitMax   = 8;



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
// DETACHED_KEEP_MS - how long the token of a request that stopped waiting is kept
//
// A request that stopped waiting answered 202 and wrote its value; the reply is
// still welcome, and lands in the attribute as an ordinary one when it comes.
// The token is kept only to hold such a reply back until the request's own
// write is done (see SyncDetached) - after this long a service has, to every
// purpose, not answered, and forgetting the token bounds what one that never
// answers can cost. A reply after that is simply an ordinary one.
//
#define DETACHED_KEEP_MS  (60 * 1000)



// -----------------------------------------------------------------------------
//
// RELEASE_WAIT_MS - how long a late reply waits for its request's write
//
// The write normally follows the timeout by a millisecond. This is the bound for
// a handler that, through some fault, never says its write is done: the reply
// lands anyway, a second late, rather than never.
//
#define RELEASE_WAIT_MS  1000



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
// ⭐ SyncDetached IS WHAT KEEPS A LATE REPLY FROM BEING LOST. The request stopped
// waiting BEFORE its own write: were the reply written the moment it came, it
// could land on the attribute's OLD value, and the request's write - a moment
// later, replacing the instance - would take it away again. So a reply for a
// detached request waits for the request to say its write is done (released).
//
typedef enum SyncState
{
  SyncWaiting   = 0,                                  // invoked, the request waits
  SyncAnswered  = 1,                                  // the reply is here, for the waiter to take
  SyncDetached  = 2,                                  // the request stopped waiting (202) and has not written yet
  SyncReleased  = 3                                   // ... and now has - a reply lands as an ordinary one
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

  int64_t             detachedAtMs;
  struct SyncWaiter*  next;
} SyncWaiter;

static pthread_mutex_t  syncMutex   = PTHREAD_MUTEX_INITIALIZER;
static SyncWaiter*      waiters     = NULL;
static uint64_t         nextToken   = 1;
static int              waitingNow  = 0;              // requests waiting right now - capped by bridgeSyncWaitMax



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
// waiterSweep - forget the tokens of requests that stopped waiting long ago
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

    if (((wP->state == SyncDetached) || (wP->state == SyncReleased)) && (now - wP->detachedAtMs > DETACHED_KEEP_MS))
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
// sendError - the answer to a request whose request to the DDS side did not go
//
static void sendError(int r, const char* attrName, const Channel* channelP, const char* what)
{
  if (r == BRIDGE_BAD_INPUT)
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid request",
            "the value of '%s' does not fit the %s of '%s'", attrName, what, channelP->endpoint);
  else if (r == BRIDGE_UNSUPPORTED)
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Operation Not Supported",
            "bridge '%s' cannot send the %s of '%s'", channelP->bridgeName, what, channelP->endpoint);
  else
    ldError(503, LD_ERROR_INTERNAL_ERROR, "Service Unavailable",
            "'%s' on bridge '%s' could not be reached - nothing was written", channelP->endpoint, channelP->bridgeName);
}



// -----------------------------------------------------------------------------
//
// SyncOutcome - what came of invoking a service and waiting for it
//
typedef enum SyncOutcome
{
  SyncFailed    = 0,                                  // not sent - the error is set
  SyncReplied   = 1,                                  // the reply came in time - *waiterPP holds it
  SyncTimedOut  = 2                                   // sent, no reply in time - *tokenP is detached
} SyncOutcome;



// -----------------------------------------------------------------------------
//
// syncInvoke - invoke one service and wait, a short while, for its reply
//
static SyncOutcome syncInvoke(const char* entityId, const char* attrName, Channel* channelP, const char* json, SyncWaiter** waiterPP, uint64_t* tokenP)
{
  BridgeDriver* driverP = driverFor(channelP);

  if ((driverP == NULL) || (driverP->serviceInvokeTracked == NULL))
  {
    sendError(BRIDGE_UNSUPPORTED, attrName, channelP, "tracked request");
    return SyncFailed;
  }

  SyncWaiter* wP = waiterCreate();

  if (wP == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "out of memory");
    return SyncFailed;
  }

  int r = driverP->serviceInvokeTracked(channelP->endpoint, json, wP->token);

  if (r != BRIDGE_OK)
  {
    pthread_mutex_lock(&syncMutex);
    waiterUnlink(wP);
    waiterFree(wP);
    pthread_mutex_unlock(&syncMutex);

    sendError(r, attrName, channelP, "request");
    return SyncFailed;
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
    // The request went, and is not finished: that is 202, not an error. Left in
    // the list, detached, so that the reply - still welcome - waits for this
    // request's write before it lands. By token from here on: the entry is no
    // longer this thread's to point at.
    //
    wP->state        = SyncDetached;
    wP->detachedAtMs = nowMs();
    *tokenP          = wP->token;
    pthread_mutex_unlock(&syncMutex);

    KT_T(KtBridge, "service '%s' did not answer within %d ms - accepted (202), its reply will land when it comes",
         channelP->endpoint, bridgeSyncTimeoutMs);
    return SyncTimedOut;
  }

  waiterUnlink(wP);
  pthread_mutex_unlock(&syncMutex);

  *waiterPP = wP;
  return SyncReplied;
}



// -----------------------------------------------------------------------------
//
// waitSlotTake / waitSlotGive - the cap on requests waiting at once
//
// ⭐ A WAITING REQUEST HOLDS A WORKER, and the pool is small (--connectionPoolSize).
// A DDS network that is slow, or gone, would otherwise take every worker in turn
// and the broker would stop answering anything at all. So only so many may wait;
// the rest send, and answer 202 at once.
//
static bool waitSlotTake(void)
{
  bool taken = false;

  pthread_mutex_lock(&syncMutex);
  if (waitingNow < bridgeSyncWaitMax)
  {
    ++waitingNow;
    taken = true;
  }
  pthread_mutex_unlock(&syncMutex);

  return taken;
}

static void waitSlotGive(void)
{
  pthread_mutex_lock(&syncMutex);
  --waitingNow;
  pthread_mutex_unlock(&syncMutex);
}



// -----------------------------------------------------------------------------
//
// doneAdd - note a Channel this request has already sent to
//
static bool doneAdd(BridgeSyncDone* doneP, Channel* channelP, uint64_t detachedToken, uint64_t goalToken)
{
  if (doneP->count >= BRIDGE_SYNC_MAX)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid request",
            "one request may send to at most %d services and actions", BRIDGE_SYNC_MAX);
    return false;
  }

  doneP->channelV[doneP->count]      = channelP;
  doneP->detachedV[doneP->count]     = detachedToken;
  doneP->goalV[doneP->count]         = goalToken;
  doneP->count++;

  return true;
}



// -----------------------------------------------------------------------------
//
// requestsFailed - the request fails after some of its requests went out
//
// It writes nothing, so it will not say "written" - but what it already sent
// must not wait for a write that never comes: held goals and detached replies
// are released at once. Always returns false, for the caller to return.
//
static bool requestsFailed(BridgeSyncDone* doneP)
{
  bridgeRequestsWritten(doneP);
  return false;
}



// -----------------------------------------------------------------------------
//
// bridgeRequestsBeforeWrite -
//
bool bridgeRequestsBeforeWrite(Tenant* tenantP, const char* entityId, KjNode* fragmentP, BridgeSyncDone* doneP)
{
  doneP->count    = 0;
  doneP->accepted = false;

  if ((bridgeCount == 0) || (channelCount() == 0) || (entityId == NULL) || (fragmentP == NULL))
    return true;

  bool wait;

  if (bridgeSyncRequested(&wait) == false)
    return requestsFailed(doneP);

  for (KjNode* attrP = fragmentP->value.firstChildP; attrP != NULL; attrP = attrP->next)
  {
    if (ldIsNotAttributeName(attrP->name) == true)
      continue;

    Channel* channelP = channelLookupByTarget(tenantP, entityId, attrP->name);

    if ((channelP == NULL) || (channelP->kind == BridgeChannelTopic))
      continue;

    if ((channelP->direction == BridgeDirectionIn) || (channelP->status != ChannelStatusAvailable))
      continue;

    //
    // The default instance only - it is what a service is sent and what a goal
    // is made of, as after the write (bridgeAttrOut).
    //
    KjNode* instanceP = kjLookup(attrP, "@none");
    KjNode* valueP    = (instanceP != NULL) ? kjLookup(instanceP, "value") : NULL;

    if (valueP == NULL)
      continue;

    static __thread char buf[SYNC_OUT_MAX];
    char*   savedName = valueP->name;
    KjNode* savedNext = valueP->next;

    valueP->name = NULL;
    valueP->next = NULL;
    kjFastRender(valueP, buf);
    valueP->name = savedName;
    valueP->next = savedNext;

    //
    // An action: the goal is sent, and can only be accepted - what becomes of it
    // comes later, into an instance of its own.
    //
    if (channelP->kind == BridgeChannelAction)
    {
      uint64_t goalToken = 0;
      int      r         = bridgeGoalSend(channelP, buf, true, &goalToken);   // held until this request has written

      if (r != BRIDGE_OK)
      {
        sendError(r, attrP->name, channelP, "goal");
        return requestsFailed(doneP);
      }

      doneP->accepted = true;

      if (doneAdd(doneP, channelP, 0, goalToken) == false)
      {
        bridgeGoalRelease(goalToken);
        return requestsFailed(doneP);
      }

      continue;
    }

    //
    // A service nobody waits for - not asked to, or no wait slot free - is sent
    // now, and the request answers that it was: 202.
    //
    if ((wait == false) || (waitSlotTake() == false))
    {
      BridgeDriver* driverP = driverFor(channelP);
      int           r       = ((driverP == NULL) || (driverP->serviceInvoke == NULL)) ? BRIDGE_UNSUPPORTED : driverP->serviceInvoke(channelP->endpoint, buf);

      if (r != BRIDGE_OK)
      {
        sendError(r, attrP->name, channelP, "request");
        return requestsFailed(doneP);
      }

      if (wait == true)
        KT_T(KtBridge, "%s/%s asks service '%s' without waiting - %d requests wait already",
             entityId, attrP->name, channelP->endpoint, bridgeSyncWaitMax);

      doneP->accepted = true;

      if (doneAdd(doneP, channelP, 0, 0) == false)
        return requestsFailed(doneP);

      continue;
    }

    SyncWaiter* wP    = NULL;
    uint64_t    token = 0;
    SyncOutcome o     = syncInvoke(entityId, attrP->name, channelP, buf, &wP, &token);

    waitSlotGive();

    if (o == SyncFailed)
      return requestsFailed(doneP);

    if (o == SyncTimedOut)
    {
      doneP->accepted = true;

      if (doneAdd(doneP, channelP, token, 0) == false)
        return requestsFailed(doneP);

      continue;
    }

    KjNode* replyP = bridgeReplySubAttr(attrP->name, wP->subAttrName, wP->json, wP->publishTime);

    if (replyP == NULL)
    {
      ldError(502, LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
              "service '%s' on bridge '%s' answered with something that is not JSON",
              channelP->endpoint, channelP->bridgeName);
      waiterFree(wP);
      return requestsFailed(doneP);
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
    waiterFree(wP);

    if (doneAdd(doneP, channelP, 0, 0) == false)
      return requestsFailed(doneP);
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// bridgeRequestsWritten -
//
void bridgeRequestsWritten(const BridgeSyncDone* doneP)
{
  if ((doneP == NULL) || (doneP->count == 0))
    return;

  for (int ix = 0; ix < doneP->count; ix++)
  {
    if (doneP->goalV[ix] != 0)
      bridgeGoalRelease(doneP->goalV[ix]);
  }

  pthread_mutex_lock(&syncMutex);
  for (int ix = 0; ix < doneP->count; ix++)
  {
    if (doneP->detachedV[ix] == 0)
      continue;

    for (SyncWaiter* wP = waiters; wP != NULL; wP = wP->next)
    {
      if ((wP->token == doneP->detachedV[ix]) && (wP->state == SyncDetached))
      {
        wP->state = SyncReleased;
        pthread_cond_broadcast(&wP->cond);            // a reply may be waiting for exactly this
        break;
      }
    }
  }
  pthread_mutex_unlock(&syncMutex);
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

    //
    // Late: the request answered 202. Its reply is welcome, and lands as an
    // ordinary one - but not before the request's own write, or the write would
    // replace it away (see SyncDetached). Bounded, in case the release never
    // comes.
    //
    if ((wP != NULL) && ((wP->state == SyncDetached) || (wP->state == SyncReleased)))
    {
      struct timespec deadline;
      int64_t         dueMs = nowMs() + RELEASE_WAIT_MS;

      deadline.tv_sec  = dueMs / 1000;
      deadline.tv_nsec = (dueMs % 1000) * 1000000;

      while (wP->state == SyncDetached)
      {
        if (pthread_cond_timedwait(&wP->cond, &syncMutex, &deadline) == ETIMEDOUT)
          break;
      }

      waiterUnlink(wP);
      waiterFree(wP);
      pthread_mutex_unlock(&syncMutex);

      KT_T(KtBridge, "bridge '%s': service '%s' answered late - written as an ordinary reply",
           (bridgeName != NULL) ? bridgeName : "?", (endpoint != NULL) ? endpoint : "?");

      return bridgeSampleQualifiedIn(bridgeName, endpoint, datasetId, subAttrName, json, publishTime);
    }

    pthread_mutex_unlock(&syncMutex);
  }

  //
  // Nobody is waiting for it - an ordinary reply, to an ordinary invocation.
  //
  return bridgeSampleQualifiedIn(bridgeName, endpoint, datasetId, subAttrName, json, publishTime);
}

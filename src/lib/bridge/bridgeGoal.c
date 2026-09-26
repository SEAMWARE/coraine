//
// FILE            bridgeGoal.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <inttypes.h>                                 // PRIu64
#include <pthread.h>                                  // pthread_mutex_*
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // uint64_t, int64_t, uintptr_t
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // calloc, free
#include <string.h>                                   // strcmp, strdup, memset
#include <errno.h>                                    // ETIMEDOUT
#include <time.h>                                     // clock_gettime

#include "ktrace/kTrace.h"                            // KT_T, KT_W
#include "corBridge/BridgeDriver.h"                   // BridgeDriver, bridges, bridgeCount, BRIDGE_*
#include "bridge/Channel.h"                           // Channel
#include "bridge/channelCache.h"                      // channelLookupByTarget
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjObject, kjArray, kjString, kjChildAdd
#include "kjson/kjParse.h"                            // kjParse
#include "kalloc/kaStrdup.h"                          // kaStrdup
#include "kalloc/kaBufferInit.h"                      // kaBufferInit
#include "kalloc/kaBufferReset.h"                     // kaBufferReset
#include "kjson/kjBufferCreate.h"                     // kjBufferCreate
#include "corBridge/BridgeBroker.h"                   // BridgeGoalState, BridgeGoalPart
#include "corRest/corRest.h"                          // corRest
#include "corNgsild/LdVocab.h"                        // LD_VOCAB_*
#include "corNgsild/LdSubCache.h"                     // LdSubCache
#include "corNgsild/ldSubCache.h"                     // ldSubCacheItemAdd, ldSubCacheItemRemove, ldSubCacheWrLock
#include "bridge/bridgeSampleIn.h"                    // bridgeGoalWrite, bridgeGoalInstanceRemove
#include "bridge/bridgeServiceSync.h"                 // bridgeSyncTimeoutMs
#include "bridge/bridgeGoal.h"                        // Own interface
#include "coraineTraceLevels.h"                       // KtBridge



// -----------------------------------------------------------------------------
//
// GOAL_TTL_MS - how long a goal may go without its final event before it is let go
//
// The plugin promises exactly one final event per goal, and a server can still
// vanish in a way no transport reports. A goal that nobody will ever finish is
// dropped from the registry after this long - its instance stays, as the
// record of a goal whose end was never heard.
//
#define GOAL_TTL_MS  (60 * 60 * 1000)



// -----------------------------------------------------------------------------
//
// HELD_WAIT_MS - how long an event of a held goal waits for the request's write, beyond --ddsSyncTimeout
//
// The first event of a goal is handed to the request that sent it, and the
// write follows it. A later event can arrive before that write - and in a batch,
// the write waits for EVERY goal of the batch, up to --ddsSyncTimeout. So an
// event of a held goal waits that long, and this on top of it, which bounds a
// handler that never says it has written.
//
#define HELD_WAIT_MS  1000



// -----------------------------------------------------------------------------
//
// Goal - one goal in flight
//
// Everything malloc'd: the registry outlives the request that sent the goal, and
// the events arrive on plugin threads whose arenas are reset per write.
//
typedef struct Goal
{
  uint64_t      token;
  char*         bridgeName;
  char*         endpoint;
  Tenant*       tenantP;
  char*         entityId;
  char*         attrName;
  char*         request;                              // the goal as sent - its instance's value
  char*         goalId;                               // the transport's, once an event has said
  char*         goalAlias;                            // the instance's datasetId, once an event has said
  char*         notifyEndpoint;                       // where the goal's events are notified - NULL: nowhere
  char*         notifyAccept;                         // ... and in what: application/json unless a default said otherwise
  char*         subId;                                // the goal's own subscription, once made
  char*         entityType;                           // for that subscription's entity selector
  int           state;                                // BridgeGoalState
  bool          instanceMade;                         // an event has been written into the instance
  bool          held;                                 // sent before its request's write, which has not happened yet
  int64_t       sentMs;
  char*         feedback;                             // the latest feedback, as the plugin sent it (part: ABI 5)
  char*         result;                               // the result, once it came (part: ABI 5)
  bool          answered;                             // its first event has arrived - accepted, or not
  bool          firstFinal;                           // ... and it was the goal's last
  bool          endPending;                           // ended with its first event: the instance goes once the request has written it
  char*         firstSubAttr;                         // ... and what it carried - for the request's write
  char*         firstJson;
  char*         firstMeta;                            // ... and the transport's meta about it (ABI 6)
  int64_t       firstTime;
  struct Goal*  next;
} Goal;



// -----------------------------------------------------------------------------
//
// Module state
//
// ⭐ ONE LOCK, HELD FOR THE WHOLE OF AN EVENT, write included. The events of one
// goal may arrive on several plugin threads at once - a transport delivers
// feedback, status and result on threads of its own - and two of them must not
// both find the instance missing and create it, nor may an event be written
// after the final one has removed the instance, which would bring back a goal
// that has ended as an orphan instance nobody will ever remove. The plugin
// promises that final comes last; this is what makes it true on the broker's
// side as well, whatever the threads do.
//
// Never held while the PLUGIN is called: a plugin may report on a goal from
// inside actionGoalSend() or actionGoalCancel(), and would then wait for itself.
//
static Goal*            goals     = NULL;
static uint64_t         nextToken = 1;
static pthread_mutex_t  goalMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   goalReleased;                 // CLOCK_MONOTONIC - initialised on first use
static bool             goalReleasedInit = false;
static pthread_cond_t   goalAnswered;                 // CLOCK_MONOTONIC - initialised with goalReleased

static Goal* goalByToken(uint64_t token);
static void  goalEnd(Goal* goalP);



// -----------------------------------------------------------------------------
//
// nowMs -
//
static int64_t nowMs(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}



// -----------------------------------------------------------------------------
//
// driverFor - the loaded bridge that carries a Channel
//
static BridgeDriver* driverFor(const char* bridgeName)
{
  for (int ix = 0; ix < bridgeCount; ix++)
  {
    if ((bridges[ix].alias != NULL) && (strcmp(bridges[ix].alias, bridgeName) == 0))
      return &bridges[ix];
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// goalFree -
//
static void goalFree(Goal* goalP)
{
  free(goalP->bridgeName);
  free(goalP->endpoint);
  free(goalP->entityId);
  free(goalP->attrName);
  free(goalP->request);
  free(goalP->goalId);
  free(goalP->goalAlias);
  free(goalP->notifyEndpoint);
  free(goalP->notifyAccept);
  free(goalP->subId);
  free(goalP->entityType);
  free(goalP->feedback);
  free(goalP->result);
  free(goalP->firstSubAttr);
  free(goalP->firstJson);
  free(goalP->firstMeta);
  free(goalP);
}



// -----------------------------------------------------------------------------
//
// goalUnlink - take a goal out of the registry. Caller holds goalMutex.
//
static void goalUnlink(Goal* goalP)
{
  for (Goal** prevPP = &goals; *prevPP != NULL; prevPP = &(*prevPP)->next)
  {
    if (*prevPP == goalP)
    {
      *prevPP = goalP->next;
      return;
    }
  }
}



// -----------------------------------------------------------------------------
//
// goalSubscribe - the goal's own subscription, for the endpoint its request named
//
// Made on the goal's FIRST event, before that event is written - which is when
// the goal's alias, the datasetId of its instance, is known - so that the event
// itself is notified. It watches attr@alias (corNgsild: a watchedAttributes
// entry naming one instance), so the endpoint hears THIS goal and no other on
// the same attribute, and projects datasetId to it, so it receives that instance
// alone. The triggers are the attribute's three: the instance is created,
// written, and removed.
//
// ⭐ IN THE CACHE ONLY. It is never stored, never listed by GET /subscriptions,
// and gone with the goal (goalUnsubscribe) - or with the broker, like the goal
// itself. The periodic statistics flush reaches it like any cached
// subscription: mongoc's is an update that matches nothing, corDB has none.
//
// The tree is built in an arena of its own and dropped once the cache has cloned
// it. corRest.kjsonP is no arena here: a goal event arrives on a plugin thread
// that may never have handled a sample (threadBind, bridgeSampleIn.c), and there
// every node was a malloc of its own, never freed - 1,301 bytes per goal with an
// endpoint (nightly valgrind, bridge_action_goal_*).
//
// Caller holds goalMutex.
//
static void goalSubscribe(Goal* goalP)
{
  if ((goalP->notifyEndpoint == NULL) || (goalP->subId != NULL) || (goalP->goalAlias == NULL))
    return;

  LdSubCache* cacheP = (LdSubCache*) goalP->tenantP->subCacheP;

  if (cacheP == NULL)
    return;

  char subId[128];
  snprintf(subId, sizeof(subId), "urn:coraine:goal-subscription:%" PRIu64, goalP->token);

  int   watchedLen = strlen(goalP->attrName) + 1 + strlen(goalP->goalAlias) + 1;
  char* watched    = (char*) malloc(watchedLen);

  if (watched == NULL)
    return;

  snprintf(watched, watchedLen, "%s@%s", goalP->attrName, goalP->goalAlias);

  char    kaBuffer[4096];
  KAlloc  kalloc;
  Kjson   kjson;

  kaBufferInit(&kalloc, kaBuffer, sizeof(kaBuffer), 4096, NULL, "goal-subscription");
  Kjson*  kjsonP = kjBufferCreate(&kjson, &kalloc);

  KjNode* subP      = kjObject(kjsonP, NULL);
  KjNode* entitiesP = kjArray(kjsonP, LD_VOCAB_ENTITIES);
  KjNode* selectorP = kjObject(kjsonP, NULL);
  KjNode* watchedP  = kjArray(kjsonP, LD_VOCAB_WATCHED_ATTRS);
  KjNode* datasetP  = kjArray(kjsonP, LD_VOCAB_DATASET_ID);
  KjNode* triggerP  = kjArray(kjsonP, "notificationTrigger");
  KjNode* notifP    = kjObject(kjsonP, LD_VOCAB_NOTIFICATION);
  KjNode* endpointP = kjObject(kjsonP, LD_VOCAB_ENDPOINT);

  kjChildAdd(subP, kjString(kjsonP, "id",   subId));
  kjChildAdd(subP, kjString(kjsonP, "type", "Subscription"));

  kjChildAdd(selectorP, kjString(kjsonP, "id", goalP->entityId));
  if (goalP->entityType != NULL)
    kjChildAdd(selectorP, kjString(kjsonP, "type", goalP->entityType));
  kjChildAdd(entitiesP, selectorP);
  kjChildAdd(subP, entitiesP);

  kjChildAdd(watchedP, kjString(kjsonP, NULL, watched));
  kjChildAdd(subP, watchedP);

  kjChildAdd(datasetP, kjString(kjsonP, NULL, goalP->goalAlias));
  kjChildAdd(subP, datasetP);

  kjChildAdd(triggerP, kjString(kjsonP, NULL, "attributeCreated"));
  kjChildAdd(triggerP, kjString(kjsonP, NULL, "attributeUpdated"));
  kjChildAdd(triggerP, kjString(kjsonP, NULL, "attributeDeleted"));
  kjChildAdd(subP, triggerP);

  kjChildAdd(endpointP, kjString(kjsonP, LD_VOCAB_URI, goalP->notifyEndpoint));
  kjChildAdd(endpointP, kjString(kjsonP, "accept", (goalP->notifyAccept != NULL) ? goalP->notifyAccept : "application/json"));
  kjChildAdd(notifP, endpointP);
  kjChildAdd(subP, notifP);

  ldSubCacheWrLock(cacheP);
  LdSubCacheItem* itemP = ldSubCacheItemAdd(cacheP, subP, NULL, LdFormatUnset);   // clones the tree
  ldSubCacheUnlock(cacheP);

  kaBufferReset(&kalloc, KFALSE);   // the tree - the cache holds its own clone
  free(watched);

  if (itemP == NULL)
  {
    KT_W("goal %" PRIu64 ": its subscription for '%s' could not be made - its events go unnotified", goalP->token, goalP->notifyEndpoint);
    return;
  }

  goalP->subId = strdup(subId);
  KT_T(KtBridge, "goal %" PRIu64 " (%s): events notified to %s", goalP->token, goalP->goalAlias, goalP->notifyEndpoint);
}



// -----------------------------------------------------------------------------
//
// goalUnsubscribe - the goal has ended: its subscription goes. Caller holds goalMutex.
//
// A notification of the goal's last write still being sent is not disturbed:
// the sender pins the cache item, and a removed item waits on the retired list
// until it is unpinned.
//
static void goalUnsubscribe(Goal* goalP)
{
  if (goalP->subId == NULL)
    return;

  LdSubCache* cacheP = (LdSubCache*) goalP->tenantP->subCacheP;

  if (cacheP != NULL)
  {
    ldSubCacheWrLock(cacheP);
    ldSubCacheItemRemove(cacheP, goalP->subId);
    ldSubCacheUnlock(cacheP);
  }

  free(goalP->subId);
  goalP->subId = NULL;
}



// -----------------------------------------------------------------------------
//
// goalSweep - let go of goals whose end was never heard. Caller holds goalMutex.
//
static void goalSweep(void)
{
  int64_t now   = nowMs();
  Goal*   goalP = goals;

  while (goalP != NULL)
  {
    Goal* nextP = goalP->next;

    if (now - goalP->sentMs > GOAL_TTL_MS)
    {
      KT_W("goal %" PRIu64 " on '%s' (%s) has not ended in %d minutes - no longer followed",
           goalP->token, goalP->endpoint, (goalP->goalAlias != NULL) ? goalP->goalAlias : "no alias yet", GOAL_TTL_MS / 60000);
      goalUnsubscribe(goalP);
      goalUnlink(goalP);
      goalFree(goalP);
    }

    goalP = nextP;
  }
}



// -----------------------------------------------------------------------------
//
// goalLookup - a goal in flight, by token. Caller holds goalMutex.
//
static Goal* goalLookup(const char* bridgeName, uint64_t token)
{
  Goal* goalP = goals;

  while ((goalP != NULL) && ((goalP->token != token) || (strcmp(goalP->bridgeName, bridgeName) != 0)))
    goalP = goalP->next;

  return goalP;
}



// -----------------------------------------------------------------------------
//
// releasedCondInit - the condition held events wait on. Caller holds goalMutex.
//
static void releasedCondInit(void)
{
  if (goalReleasedInit == true)
    return;

  pthread_condattr_t attr;

  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&goalReleased, &attr);
  pthread_cond_init(&goalAnswered, &attr);
  pthread_condattr_destroy(&attr);

  goalReleasedInit = true;
}



// -----------------------------------------------------------------------------
//
// NotifyDefault - a Bridge's default goal endpoint, from its configuration
//
// Where a goal is notified, the first that exists (bridge-channels.md 9.1):
//   1. the goal's own endpoint - the request's "endpoint"
//   2. its Channel's default   - Channel::notifyUri
//   3. its Bridge's default    - this table
//   4. none: the goal is polled
//
// A default does not make a subscription of its own: a goal that falls back to
// one is given it as its endpoint, and gets the same per-goal subscription an
// endpoint of its own would. So a default hears the goals that named none -
// never a goal with an endpoint of its own, nor an ordinary write of the
// attribute.
//
// ⭐ Written only while the configuration loads, before any request or plugin
// thread exists, and read-only after - so no lock.
//
#define NOTIFY_DEFAULTS_MAX  16

typedef struct NotifyDefault
{
  char* bridgeName;
  char* uri;
  char* accept;
} NotifyDefault;

static NotifyDefault notifyDefaults[NOTIFY_DEFAULTS_MAX];
static int           notifyDefaultCount = 0;



// -----------------------------------------------------------------------------
//
// bridgeGoalNotifyDefaultSet - a Bridge's default goal endpoint (startup only)
//
void bridgeGoalNotifyDefaultSet(const char* bridgeName, const char* uri, const char* accept)
{
  for (int ix = 0; ix < notifyDefaultCount; ix++)
  {
    if (strcmp(notifyDefaults[ix].bridgeName, bridgeName) == 0)
    {
      free(notifyDefaults[ix].uri);
      free(notifyDefaults[ix].accept);
      notifyDefaults[ix].uri    = strdup(uri);
      notifyDefaults[ix].accept = (accept != NULL) ? strdup(accept) : NULL;
      return;
    }
  }

  if (notifyDefaultCount >= NOTIFY_DEFAULTS_MAX)
  {
    KT_W("bridge '%s': no room for its default goal endpoint - its goals are notified only where they say", bridgeName);
    return;
  }

  notifyDefaults[notifyDefaultCount].bridgeName = strdup(bridgeName);
  notifyDefaults[notifyDefaultCount].uri        = strdup(uri);
  notifyDefaults[notifyDefaultCount].accept     = (accept != NULL) ? strdup(accept) : NULL;
  ++notifyDefaultCount;
}



// -----------------------------------------------------------------------------
//
// bridgeGoalNotifyDefault - a Bridge's default goal endpoint, or false
//
bool bridgeGoalNotifyDefault(const char* bridgeName, const char** uriP, const char** acceptP)
{
  for (int ix = 0; ix < notifyDefaultCount; ix++)
  {
    if (strcmp(notifyDefaults[ix].bridgeName, bridgeName) == 0)
    {
      *uriP    = notifyDefaults[ix].uri;
      *acceptP = notifyDefaults[ix].accept;
      return true;
    }
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// goalNotifyChoose - where this goal's events go: its own endpoint, its Channel's, its Bridge's, or nowhere
//
static void goalNotifyChoose(Goal* goalP, Channel* channelP, const char* endpoint)
{
  const char* uri    = endpoint;
  const char* accept = NULL;

  if ((uri == NULL) && (channelP->notifyUri != NULL))
  {
    uri    = channelP->notifyUri;
    accept = channelP->notifyAccept;
  }

  if (uri == NULL)
    bridgeGoalNotifyDefault(channelP->bridgeName, &uri, &accept);

  goalP->notifyEndpoint = (uri    != NULL) ? strdup(uri)    : NULL;
  goalP->notifyAccept   = (accept != NULL) ? strdup(accept) : NULL;
}



// -----------------------------------------------------------------------------
//
// bridgeGoalSend -
//
int bridgeGoalSend(Channel* channelP, const char* json, const char* endpoint, uint64_t* tokenP)
{
  BridgeDriver* driverP = driverFor(channelP->bridgeName);

  if ((driverP == NULL) || (driverP->actionGoalSend == NULL))
  {
    KT_W("bridge '%s' carries no goals - the write to %s/%s went nowhere",
         channelP->bridgeName, channelP->entityId, channelP->attrName);
    return BRIDGE_UNSUPPORTED;
  }

  Goal* goalP = (Goal*) calloc(1, sizeof(Goal));

  if (goalP == NULL)
    return BRIDGE_ERR;

  goalP->bridgeName = strdup(channelP->bridgeName);
  goalP->endpoint   = strdup(channelP->endpoint);
  goalP->tenantP    = channelP->tenantP;
  goalP->entityId   = strdup(channelP->entityId);
  goalP->attrName   = strdup(channelP->attrName);
  goalP->request    = strdup(json);
  goalNotifyChoose(goalP, channelP, endpoint);
  goalP->entityType = (channelP->entityType != NULL) ? strdup(channelP->entityType) : NULL;
  goalP->state      = BridgeGoalUnknown;
  goalP->held       = true;                         // its request has not written yet - see bridgeGoalRelease
  goalP->sentMs     = nowMs();

  //
  // Registered BEFORE the plugin is called - an event may come back before
  // actionGoalSend() has returned, and it has to find its goal.
  //
  pthread_mutex_lock(&goalMutex);
  goalSweep();
  goalP->token = nextToken++;
  goalP->next  = goals;
  goals        = goalP;
  uint64_t token = goalP->token;
  pthread_mutex_unlock(&goalMutex);

  int r = driverP->actionGoalSend(channelP->endpoint, json, token);

  if (r != BRIDGE_OK)
  {
    //
    // Nothing will come for it (the contract says so), so it is let go at once.
    // By token, not by pointer - the goal is not the caller's any more.
    //
    pthread_mutex_lock(&goalMutex);
    for (Goal* gP = goals; gP != NULL; gP = gP->next)
    {
      if (gP->token == token)
      {
        goalUnlink(gP);
        goalFree(gP);
        break;
      }
    }
    pthread_mutex_unlock(&goalMutex);

    return r;
  }

  KT_T(KtBridge, "%s/%s sends goal %" PRIu64 " to action '%s' on bridge '%s'",
       channelP->entityId, channelP->attrName, token, channelP->endpoint, channelP->bridgeName);

  if (tokenP != NULL)
    *tokenP = token;

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// goalEndThread - the end of a goal that ended with its first event
//
static void* goalEndThread(void* arg)
{
  uint64_t token = (uint64_t) (uintptr_t) arg;

  pthread_mutex_lock(&goalMutex);

  Goal* goalP = goalByToken(token);

  if (goalP != NULL)
    goalEnd(goalP);

  pthread_mutex_unlock(&goalMutex);

  return NULL;
}



// -----------------------------------------------------------------------------
//
// goalEndLater - a goal over with its first event: its instance goes, now that the request has written it
//
// ⭐ WRITTEN AND THEN REMOVED, not skipped. KZ: the temporal history needs the
// goal - that it was asked, and how it ended - and a subscriber, or the goal's
// own endpoint, must hear it. The request's write made the instance; this
// removes it exactly as any ended goal's is removed. Only current state does
// without it, as it does without every goal that has ended.
//
// ⚠ On a thread of its own, never the request's. The removal is a write of its
// own, and it drains the notification and TRoE queues and frees the thread's
// per-thread state as a plugin thread's events do (bridgeGoalInstanceRemove) -
// on a request thread that would dispatch the request's own notifications
// before its response. Rare - a goal that ends before it was ever in progress.
//
// Caller holds goalMutex.
//
static void goalEndLater(Goal* goalP)
{
  pthread_t      tid;
  pthread_attr_t attr;

  goalP->endPending = false;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  if (pthread_create(&tid, &attr, goalEndThread, (void*) (uintptr_t) goalP->token) != 0)
    KT_W("goal %" PRIu64 " on '%s': ended, but its instance cannot be removed (no thread) - it stays", goalP->token, goalP->endpoint);

  pthread_attr_destroy(&attr);
}



// -----------------------------------------------------------------------------
//
// bridgeGoalRelease -
//
void bridgeGoalRelease(uint64_t token)
{
  pthread_mutex_lock(&goalMutex);

  Goal* goalP = goalByToken(token);

  if (goalP != NULL)
  {
    goalP->held = false;

    if (goalP->endPending == true)
      goalEndLater(goalP);
  }

  releasedCondInit();
  pthread_cond_broadcast(&goalReleased);
  pthread_mutex_unlock(&goalMutex);
}



// -----------------------------------------------------------------------------
//
// goalIdentify - the transport's id and the instance's datasetId, from the first event that says. Caller holds goalMutex.
//
static void goalIdentify(Goal* goalP, const char* goalId, const char* goalAlias)
{
  if ((goalP->goalId == NULL) && (goalId != NULL))
    goalP->goalId = strdup(goalId);

  //
  // The instance needs a datasetId, and a plugin that gives none still gets one:
  // minted from the token, which is unique for as long as this broker runs.
  //
  if (goalP->goalAlias == NULL)
  {
    if (goalAlias != NULL)
      goalP->goalAlias = strdup(goalAlias);
    else
    {
      char alias[64];

      snprintf(alias, sizeof(alias), "urn:coraine:goal:%" PRIu64, goalP->token);
      goalP->goalAlias = strdup(alias);
    }
  }
}



// -----------------------------------------------------------------------------
//
// goalStateKeep - the state, and the payload by its part. Caller holds goalMutex.
//
// What the payload IS, when the plugin says - so a goal can be shown the same
// way whatever transport carries it (the goal resource's goalFeedback and
// goalResult). Only the latest feedback is kept: it describes the goal NOW.
//
static void goalStateKeep(Goal* goalP, int state, int part, const char* json)
{
  goalP->state = state;

  if (json == NULL)
    return;

  if (part == BridgeGoalPartFeedback)
  {
    free(goalP->feedback);
    goalP->feedback = strdup(json);
  }
  else if (part == BridgeGoalPartResult)
  {
    free(goalP->result);
    goalP->result = strdup(json);
  }
}



// -----------------------------------------------------------------------------
//
// firstEventKeep - a goal's first event, kept for the request that waits for it. Caller holds goalMutex.
//
static void firstEventKeep(Goal*       goalP,
                           const char* goalId,
                           const char* goalAlias,
                           int         state,
                           bool        final,
                           int         part,
                           const char* subAttrName,
                           const char* json,
                           const char* meta,
                           int64_t     publishTime)
{
  goalIdentify(goalP, goalId, goalAlias);
  goalStateKeep(goalP, state, part, json);

  goalP->answered   = true;
  goalP->firstFinal = final;
  goalP->firstTime  = publishTime;

  if ((subAttrName != NULL) && (json != NULL))
  {
    goalP->firstSubAttr = strdup(subAttrName);
    goalP->firstJson    = strdup(json);
    goalP->firstMeta    = (meta != NULL) ? strdup(meta) : NULL;
  }
}



// -----------------------------------------------------------------------------
//
// goalEnd - the goal has ended: its subscription goes, then its instance, then the goal. Caller holds goalMutex.
//
static void goalEnd(Goal* goalP)
{
  //
  // The goal's own subscription goes FIRST, so its endpoint does not hear the
  // instance's removal - the last thing it hears is the goal's final event.
  // That is what Orion-LD does (it deletes the temporary subscription before
  // pulling the instance), and its clients expect no removal notification.
  // Ordinary subscriptions on the attribute still hear it.
  //
  goalUnsubscribe(goalP);

  //
  // Written, notified, recorded - and now the goal's instance goes. Only if it
  // was ever made: removing an instance that is not there would still be a
  // write to the attribute, and notify its watchers of nothing.
  //
  if (goalP->instanceMade == true)
    bridgeGoalInstanceRemove(goalP->bridgeName, goalP->endpoint, goalP->goalAlias);

  KT_T(KtBridge, "goal %" PRIu64 " on '%s' (%s) ended, state %d", goalP->token, goalP->endpoint, goalP->goalAlias, goalP->state);

  goalUnlink(goalP);
  goalFree(goalP);
}



// -----------------------------------------------------------------------------
//
// goalEvent - an event of a goal, with the part its payload is (BridgeGoalPartNone: not said)
//
static int goalEvent(const char* bridgeName,
                     const char* endpoint,
                     uint64_t    token,
                     const char* goalId,
                     const char* goalAlias,
                     int         state,
                     bool        final,
                     int         part,
                     const char* subAttrName,
                     const char* json,
                     const char* meta,
                     int64_t     publishTime)
{
  if ((bridgeName == NULL) || (endpoint == NULL) || (token == 0))
    return BRIDGE_BAD_INPUT;

  pthread_mutex_lock(&goalMutex);

  Goal* goalP = goalLookup(bridgeName, token);

  //
  // ⭐ THE FIRST EVENT DECIDES, AND IT IS THE REQUEST'S - never written here.
  // The transport decides before anything is stored (bridgeGoalAwait): the
  // request that sent the goal waits for this event, and writes the value it
  // was asked for only when the goal was accepted - together with this event,
  // in its own write. Rejected, nothing is written at all.
  //
  if ((goalP != NULL) && (goalP->answered == false))
  {
    firstEventKeep(goalP, goalId, goalAlias, state, final, part, subAttrName, json, meta, publishTime);

    if (bridgeGoalRefused(state) == false)
      goalSubscribe(goalP);                           // before the request's write, which is the instance's first

    pthread_cond_broadcast(&goalAnswered);
    pthread_mutex_unlock(&goalMutex);

    KT_T(KtBridge, "goal %" PRIu64 " on '%s' answered, state %d - handed to its request", token, endpoint, state);
    return BRIDGE_OK;
  }

  //
  // ⭐ A LATER EVENT WAITS FOR THE REQUEST'S WRITE. The request writes the
  // instance, and an event written before it would race that write of the same
  // attribute: at best the request's write reads as a fresh write of the goal's
  // instance (a second notification), at worst a store that replaces the
  // attribute whole takes the instance away. So an event of a held goal waits,
  // bounded, for bridgeGoalRelease.
  //
  // By TOKEN after every wake, never by a pointer kept across the wait: another
  // event of the same goal may have been the final one meanwhile, and freed it.
  //
  if ((goalP != NULL) && (goalP->held == true))
  {
    struct timespec deadline;
    int64_t         dueMs = nowMs() + bridgeSyncTimeoutMs + HELD_WAIT_MS;

    deadline.tv_sec  = dueMs / 1000;
    deadline.tv_nsec = (dueMs % 1000) * 1000000;

    releasedCondInit();

    while (((goalP = goalLookup(bridgeName, token)) != NULL) && (goalP->held == true))
    {
      if (pthread_cond_timedwait(&goalReleased, &goalMutex, &deadline) == ETIMEDOUT)
      {
        goalP = goalLookup(bridgeName, token);

        if (goalP != NULL)
          goalP->held = false;                        // never released - write anyway, a second late

        break;
      }
    }
  }

  if (goalP == NULL)
  {
    pthread_mutex_unlock(&goalMutex);
    KT_T(KtBridge, "event for goal %" PRIu64 " on '%s' - not a goal in flight, dropped", token, endpoint);
    return BRIDGE_NOT_FOUND;
  }

  goalIdentify(goalP, goalId, goalAlias);
  goalStateKeep(goalP, state, part, json);

  //
  // A state change alone has nothing to write. A payload goes into its
  // sub-attribute - the first one creating the instance, with the request as
  // its value.
  //
  if ((subAttrName != NULL) && (json != NULL))
  {
    int r = bridgeGoalWrite(goalP->bridgeName, goalP->endpoint, goalP->goalAlias, subAttrName, json, publishTime, goalP->request, meta);

    if (r == BRIDGE_OK)
      goalP->instanceMade = true;
    else
      KT_W("goal %" PRIu64 " on '%s': its '%s' could not be written (%d)", token, endpoint, subAttrName, r);
  }

  if (final == true)
    goalEnd(goalP);

  pthread_mutex_unlock(&goalMutex);

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// bridgeGoalEventIn - ABI 4: the part is not said
//
int bridgeGoalEventIn(const char* bridgeName,
                      const char* endpoint,
                      uint64_t    token,
                      const char* goalId,
                      const char* goalAlias,
                      int         state,
                      bool        final,
                      const char* subAttrName,
                      const char* json,
                      int64_t     publishTime)
{
  return goalEvent(bridgeName, endpoint, token, goalId, goalAlias, state, final, BridgeGoalPartNone, subAttrName, json, NULL, publishTime);
}



// -----------------------------------------------------------------------------
//
// bridgeGoalEventPartIn - ABI 5
//
int bridgeGoalEventPartIn(const char* bridgeName,
                          const char* endpoint,
                          uint64_t    token,
                          const char* goalId,
                          const char* goalAlias,
                          int         state,
                          bool        final,
                          int         part,
                          const char* subAttrName,
                          const char* json,
                          int64_t     publishTime)
{
  return goalEvent(bridgeName, endpoint, token, goalId, goalAlias, state, final, part, subAttrName, json, NULL, publishTime);
}



// -----------------------------------------------------------------------------
//
// bridgeGoalEventMetaIn - ABI 6: the event's meta goes on its sub-attribute
//
int bridgeGoalEventMetaIn(const char* bridgeName,
                          const char* endpoint,
                          uint64_t    token,
                          const char* goalId,
                          const char* goalAlias,
                          int         state,
                          bool        final,
                          int         part,
                          const char* subAttrName,
                          const char* json,
                          const char* meta,
                          int64_t     publishTime)
{
  return goalEvent(bridgeName, endpoint, token, goalId, goalAlias, state, final, part, subAttrName, json, meta, publishTime);
}



// -----------------------------------------------------------------------------
//
// goalByToken - a goal in flight, by token alone. Caller holds goalMutex.
//
// Tokens are the broker's own, unique across bridges - which is why the
// request side, that knows no bridge name, can use them alone.
//
static Goal* goalByToken(uint64_t token)
{
  Goal* goalP = goals;

  while ((goalP != NULL) && (goalP->token != token))
    goalP = goalP->next;

  return goalP;
}



// -----------------------------------------------------------------------------
//
// goalDrop - a goal whose request writes nothing: out of the registry, and cancelled
//
// Taken out FIRST, under the lock, so that anything the goal still says - its
// answer arriving a moment too late, the cancel's own events - finds no goal
// and is dropped: nothing about it is ever written. Then cancelled, outside
// the lock, as the plugin may report from inside actionGoalCancel().
//
// Called with goalMutex held; returns with it released.
//
static void goalDrop(Goal* goalP, const char* why)
{
  char* bridgeName = goalP->bridgeName;
  char* endpoint   = goalP->endpoint;
  uint64_t token   = goalP->token;

  goalP->bridgeName = NULL;                           // kept for the cancel below - goalFree leaves them
  goalP->endpoint   = NULL;

  goalUnsubscribe(goalP);
  goalUnlink(goalP);
  goalFree(goalP);
  pthread_mutex_unlock(&goalMutex);

  BridgeDriver* driverP = driverFor(bridgeName);
  int           r       = ((driverP == NULL) || (driverP->actionGoalCancel == NULL))
                          ? BRIDGE_UNSUPPORTED
                          : driverP->actionGoalCancel(endpoint, token);

  KT_T(KtBridge, "goal %" PRIu64 " on '%s' %s - cancelled (%d), nothing written", token, endpoint, why, r);

  free(bridgeName);
  free(endpoint);
}



// -----------------------------------------------------------------------------
//
// bridgeGoalRefused -
//
bool bridgeGoalRefused(int state)
{
  return (state == BridgeGoalRejected) || (state == BridgeGoalFailed) || (state == BridgeGoalAborted) || (state == BridgeGoalCanceled);
}



// -----------------------------------------------------------------------------
//
// bridgeGoalAwait -
//
bool bridgeGoalAwait(uint64_t token, int64_t dueMs, BridgeGoalAnswer* answerP)
{
  struct timespec deadline;

  deadline.tv_sec  = dueMs / 1000;
  deadline.tv_nsec = (dueMs % 1000) * 1000000;

  memset(answerP, 0, sizeof(BridgeGoalAnswer));

  pthread_mutex_lock(&goalMutex);
  releasedCondInit();

  Goal* goalP;

  while (((goalP = goalByToken(token)) != NULL) && (goalP->answered == false))
  {
    if (pthread_cond_timedwait(&goalAnswered, &goalMutex, &deadline) == ETIMEDOUT)
    {
      goalP = goalByToken(token);   // the last chance - it may have come with the timeout
      break;
    }
  }

  if (goalP == NULL)
  {
    pthread_mutex_unlock(&goalMutex);   // let go meanwhile (the TTL sweep) - nothing to cancel
    return false;
  }

  if (goalP->answered == false)
  {
    goalDrop(goalP, "not answered in time");   // unlocks
    return false;
  }

  answerP->state       = goalP->state;
  answerP->final       = goalP->firstFinal;
  answerP->goalId      = (goalP->goalId       != NULL) ? kaStrdup(&corRest.kalloc, goalP->goalId)       : NULL;
  answerP->goalAlias   = kaStrdup(&corRest.kalloc, goalP->goalAlias);
  answerP->subAttrName = (goalP->firstSubAttr != NULL) ? kaStrdup(&corRest.kalloc, goalP->firstSubAttr) : NULL;
  answerP->json        = (goalP->firstJson    != NULL) ? kaStrdup(&corRest.kalloc, goalP->firstJson)    : NULL;
  answerP->meta        = (goalP->firstMeta    != NULL) ? kaStrdup(&corRest.kalloc, goalP->firstMeta)    : NULL;
  answerP->publishTime = goalP->firstTime;

  free(goalP->firstSubAttr);
  free(goalP->firstJson);
  free(goalP->firstMeta);
  goalP->firstSubAttr = NULL;
  goalP->firstJson    = NULL;
  goalP->firstMeta    = NULL;

  //
  // Refused: nothing will be written of this goal, so it leaves the registry
  // now - whatever it might still say is dropped. A refusal that is not the
  // goal's final event is taken at its word all the same.
  //
  // Accepted: the request's own write makes the instance, and later events
  // wait for that write (held) and then land in it. Over already, with this
  // first event: the request writes the instance all the same, and it is
  // removed once written (goalEndLater) - see there for why.
  //
  if (bridgeGoalRefused(answerP->state) == true)
  {
    KT_T(KtBridge, "goal %" PRIu64 " on '%s' refused, state %d - nothing written", goalP->token, goalP->endpoint, answerP->state);
    goalUnsubscribe(goalP);
    goalUnlink(goalP);
    goalFree(goalP);
  }
  else
  {
    goalP->instanceMade = true;
    goalP->endPending   = answerP->final;
  }

  pthread_mutex_unlock(&goalMutex);

  return true;
}



// -----------------------------------------------------------------------------
//
// bridgeGoalAbandon -
//
void bridgeGoalAbandon(uint64_t token)
{
  pthread_mutex_lock(&goalMutex);

  Goal* goalP = goalByToken(token);

  if (goalP == NULL)
  {
    pthread_mutex_unlock(&goalMutex);
    return;
  }

  goalDrop(goalP, "sent by a request that wrote nothing");   // unlocks
}



// -----------------------------------------------------------------------------
//
// goalStateName -
//
const char* bridgeGoalStateName(int state)
{
  switch (state)
  {
  case BridgeGoalAccepted:   return "accepted";
  case BridgeGoalExecuting:  return "executing";
  case BridgeGoalCanceling:  return "canceling";
  case BridgeGoalSucceeded:  return "succeeded";
  case BridgeGoalCanceled:   return "canceled";
  case BridgeGoalAborted:    return "aborted";
  case BridgeGoalRejected:   return "rejected";
  case BridgeGoalFailed:     return "failed";
  }

  return "unknown";
}



// -----------------------------------------------------------------------------
//
// jsonNode - a JSON text as a tree in the request's arena, as a member named 'name'
//
// A payload that does not parse is shown as the string it is rather than lost.
//
static KjNode* jsonNode(const char* name, const char* json)
{
  char*   copy  = kaStrdup(&corRest.kalloc, json);
  KjNode* nodeP = kjParse(corRest.kjsonP, copy);

  if (nodeP == NULL)
    return kjString(corRest.kjsonP, name, kaStrdup(&corRest.kalloc, json));

  nodeP->name = (char*) name;
  return nodeP;
}



// -----------------------------------------------------------------------------
//
// goalRender - a goal as its body. Caller holds goalMutex; everything is copied into the request's arena.
//
static KjNode* goalRender(Goal* goalP)
{
  Kjson*  kjsonP = corRest.kjsonP;
  KjNode* bodyP  = kjObject(kjsonP, NULL);

  kjChildAdd(bodyP, kjString(kjsonP, "id",     kaStrdup(&corRest.kalloc, goalP->goalAlias)));
  kjChildAdd(bodyP, kjString(kjsonP, "type",   "Goal"));
  kjChildAdd(bodyP, kjString(kjsonP, "goalId", kaStrdup(&corRest.kalloc, goalP->goalId)));
  kjChildAdd(bodyP, kjString(kjsonP, "status", (char*) bridgeGoalStateName(goalP->state)));

  if (goalP->request != NULL)   kjChildAdd(bodyP, jsonNode("goalRequest",  goalP->request));
  if (goalP->feedback != NULL)  kjChildAdd(bodyP, jsonNode("goalFeedback", goalP->feedback));
  if (goalP->result != NULL)    kjChildAdd(bodyP, jsonNode("goalResult",   goalP->result));

  return bodyP;
}



// -----------------------------------------------------------------------------
//
// goalOfChannel - is this goal one of the Channel's, in flight and answered?
//
static bool goalOfChannel(Goal* goalP, Channel* channelP)
{
  return (goalP->goalId != NULL)                                  &&
         (strcmp(goalP->bridgeName, channelP->bridgeName) == 0)   &&
         (strcmp(goalP->endpoint,   channelP->endpoint)   == 0);
}



// -----------------------------------------------------------------------------
//
// bridgeGoalsRender -
//
KjNode* bridgeGoalsRender(Channel* channelP)
{
  KjNode* arrayP = kjArray(corRest.kjsonP, NULL);

  pthread_mutex_lock(&goalMutex);
  for (Goal* goalP = goals; goalP != NULL; goalP = goalP->next)
  {
    if (goalOfChannel(goalP, channelP) == true)
      kjChildAdd(arrayP, goalRender(goalP));
  }
  pthread_mutex_unlock(&goalMutex);

  return arrayP;
}



// -----------------------------------------------------------------------------
//
// bridgeGoalRender -
//
KjNode* bridgeGoalRender(Channel* channelP, const char* goalId)
{
  KjNode* bodyP = NULL;

  pthread_mutex_lock(&goalMutex);
  for (Goal* goalP = goals; goalP != NULL; goalP = goalP->next)
  {
    if ((goalOfChannel(goalP, channelP) == true) && (strcmp(goalP->goalId, goalId) == 0))
    {
      bodyP = goalRender(goalP);
      break;
    }
  }
  pthread_mutex_unlock(&goalMutex);

  return bodyP;
}



// -----------------------------------------------------------------------------
//
// bridgeGoalAliasOf - the datasetId of a Channel's goal in flight, by the transport's id
//
char* bridgeGoalAliasOf(Channel* channelP, const char* goalId)
{
  char* aliasP = NULL;

  pthread_mutex_lock(&goalMutex);
  for (Goal* goalP = goals; goalP != NULL; goalP = goalP->next)
  {
    if ((goalOfChannel(goalP, channelP) == true) && (strcmp(goalP->goalId, goalId) == 0))
    {
      aliasP = kaStrdup(&corRest.kalloc, goalP->goalAlias);
      break;
    }
  }
  pthread_mutex_unlock(&goalMutex);

  return aliasP;
}



// -----------------------------------------------------------------------------
//
// bridgeGoalCancel -
//
bool bridgeGoalCancel(Tenant* tenantP, const char* entityId, const char* attrName, const char* datasetId, int* rcP)
{
  if (datasetId == NULL)
    return false;

  Channel* channelP = channelLookupByTarget(tenantP, entityId, attrName);

  if ((channelP == NULL) || (channelP->kind != BridgeChannelAction))
    return false;

  uint64_t token = 0;

  pthread_mutex_lock(&goalMutex);
  for (Goal* goalP = goals; goalP != NULL; goalP = goalP->next)
  {
    if ((goalP->goalAlias != NULL)                            &&
        (strcmp(goalP->goalAlias,  datasetId)           == 0) &&
        (strcmp(goalP->bridgeName, channelP->bridgeName) == 0) &&
        (strcmp(goalP->endpoint,   channelP->endpoint)   == 0))
    {
      token = goalP->token;
      break;
    }
  }
  pthread_mutex_unlock(&goalMutex);

  if (token == 0)
    return false;

  BridgeDriver* driverP = driverFor(channelP->bridgeName);

  *rcP = ((driverP == NULL) || (driverP->actionGoalCancel == NULL))
         ? BRIDGE_UNSUPPORTED
         : driverP->actionGoalCancel(channelP->endpoint, token);

  KT_T(KtBridge, "%s/%s asks to cancel goal %" PRIu64 " (%s) on bridge '%s' (%d)",
       entityId, attrName, token, datasetId, channelP->bridgeName, *rcP);

  return true;
}

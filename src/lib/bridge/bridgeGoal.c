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
#include <stdint.h>                                   // uint64_t, int64_t
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // calloc, free
#include <string.h>                                   // strcmp, strdup
#include <errno.h>                                    // ETIMEDOUT
#include <time.h>                                     // clock_gettime

#include "ktrace/kTrace.h"                            // KT_T, KT_W
#include "corBridge/BridgeDriver.h"                   // BridgeDriver, bridges, bridgeCount, BRIDGE_*
#include "bridge/Channel.h"                           // Channel
#include "bridge/channelCache.h"                      // channelLookupByTarget
#include "bridge/bridgeSampleIn.h"                    // bridgeGoalWrite, bridgeGoalInstanceRemove
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
// HELD_WAIT_MS - how long an event of a held goal waits for the request's write
//
// As for a late service reply (bridgeServiceSync.c): the write normally follows
// in a millisecond, and this bounds a handler that never says it has written.
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
  int           state;                                // BridgeGoalState
  bool          instanceMade;                         // an event has been written into the instance
  bool          held;                                 // sent before its request's write, which has not happened yet
  int64_t       sentMs;
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
  pthread_condattr_destroy(&attr);

  goalReleasedInit = true;
}



// -----------------------------------------------------------------------------
//
// bridgeGoalSend -
//
int bridgeGoalSend(Channel* channelP, const char* json, bool held, uint64_t* tokenP)
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
  goalP->state      = BridgeGoalUnknown;
  goalP->held       = held;
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
// bridgeGoalRelease -
//
void bridgeGoalRelease(uint64_t token)
{
  pthread_mutex_lock(&goalMutex);

  for (Goal* goalP = goals; goalP != NULL; goalP = goalP->next)
  {
    if (goalP->token == token)
    {
      goalP->held = false;
      break;
    }
  }

  releasedCondInit();
  pthread_cond_broadcast(&goalReleased);
  pthread_mutex_unlock(&goalMutex);
}



// -----------------------------------------------------------------------------
//
// bridgeGoalEventIn -
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
  if ((bridgeName == NULL) || (endpoint == NULL) || (token == 0))
    return BRIDGE_BAD_INPUT;

  pthread_mutex_lock(&goalMutex);

  //
  // ⭐ A GOAL SENT BEFORE ITS REQUEST'S WRITE WAITS FOR THAT WRITE. DDS goes
  // first, so the goal can report before the request has stored anything - and
  // an event written then would race the request's own write of the same
  // attribute: at best the request's write reads as a fresh write of the goal's
  // instance (a second notification), at worst a store that replaces the
  // attribute whole takes the instance away. So an event of a held goal waits,
  // bounded, for bridgeGoalRelease.
  //
  // By TOKEN after every wake, never by a pointer kept across the wait: another
  // event of the same goal may have been the final one meanwhile, and freed it.
  //
  Goal* goalP = goalLookup(bridgeName, token);

  if ((goalP != NULL) && (goalP->held == true))
  {
    struct timespec deadline;
    int64_t         dueMs = nowMs() + HELD_WAIT_MS;

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

      snprintf(alias, sizeof(alias), "urn:coraine:goal:%" PRIu64, token);
      goalP->goalAlias = strdup(alias);
    }
  }

  goalP->state = state;

  //
  // A state change alone has nothing to write. A payload goes into its
  // sub-attribute - the first one creating the instance, with the request as
  // its value.
  //
  if ((subAttrName != NULL) && (json != NULL))
  {
    int r = bridgeGoalWrite(goalP->bridgeName, goalP->endpoint, goalP->goalAlias, subAttrName, json, publishTime, goalP->request);

    if (r == BRIDGE_OK)
      goalP->instanceMade = true;
    else
      KT_W("goal %" PRIu64 " on '%s': its '%s' could not be written (%d)", token, endpoint, subAttrName, r);
  }

  if (final == true)
  {
    //
    // Written, notified, recorded - and now the goal's instance goes. Only if it
    // was ever made: removing an instance that is not there would still be a
    // write to the attribute, and notify its watchers of nothing.
    //
    if (goalP->instanceMade == true)
      bridgeGoalInstanceRemove(goalP->bridgeName, goalP->endpoint, goalP->goalAlias);

    KT_T(KtBridge, "goal %" PRIu64 " on '%s' (%s) ended, state %d", token, endpoint, goalP->goalAlias, state);

    goalUnlink(goalP);
    goalFree(goalP);
  }

  pthread_mutex_unlock(&goalMutex);

  return BRIDGE_OK;
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

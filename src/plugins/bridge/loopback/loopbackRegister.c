//
// FILE            loopbackRegister.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The loopback bridge - a transport that goes nowhere.
//
// It exists so that the bridge seam can be exercised without a transport
// library attached to it. What it carries out through publish() it hands
// straight back in through sampleIn(), from a thread of its own, which is the
// one property of a real bridge that matters to the broker and the one that
// unit-level testing otherwise cannot reach.
//
// It is also the reference a new bridge is written against: every entry point
// in BridgeDriver.h is implemented here, in about a page, with no dependency
// beyond libc and pthreads.
//

#include <pthread.h>                                  // pthread_create, pthread_join, pthread_mutex_*
#include <stdio.h>                                    // snprintf, fopen, fread
#include <ctype.h>                                    // isspace
#include <stdlib.h>                                   // malloc, free
#include <string.h>                                   // strcmp, strdup, memset
#include <unistd.h>                                   // usleep
#include <stdint.h>                                   // uint64_t, int64_t
#include <time.h>                                     // clock_gettime

#include "ktrace/kTrace.h"                            // KT_E

#include "corBridge/BridgeDriver.h"                   // BridgeDriver, BridgeRegisterFunc
#include "corBridge/BridgeBroker.h"                   // BridgeBroker, BRIDGE_*



// -----------------------------------------------------------------------------
//
// LOOPBACK_CHANNELS_MAX - the endpoints one loopback bridge will carry
//
#define LOOPBACK_CHANNELS_MAX 64



// -----------------------------------------------------------------------------
//
// LoopbackChannel -
//
typedef struct LoopbackChannel
{
  char*            endpoint;
  BridgeDirection  direction;
  bool             inUse;
} LoopbackChannel;



// -----------------------------------------------------------------------------
//
// Module state
//
static const BridgeBroker*  brokerP = NULL;
static LoopbackChannel      channels[LOOPBACK_CHANNELS_MAX];
static pthread_mutex_t      channelMutex = PTHREAD_MUTEX_INITIALIZER;



// -----------------------------------------------------------------------------
//
// Deferred delivery - what makes this a bridge and not a function call
//
// A real transport hands a sample back on a thread the broker knows nothing
// about, and that is the whole risk the seam exists to contain. Calling
// sampleIn() directly from publish() would deliver on the BROKER's own request
// thread and prove nothing at all - the interesting case is precisely the one
// where the caller is a stranger.
//
typedef struct LoopbackSample
{
  char*       endpoint;
  char*       json;
  const char* subAttrName;                            // NULL for a topic's sample; the reply envelope for a service
  uint64_t    token;                                  // the broker's, for a reply somebody waits for; 0 otherwise
  int64_t     dueMs;                                  // not delivered before this (monotonic ms) - see replyDelayMs
  bool        goal;                                   // an event of a goal: goalEventIn, not sampleIn
  int         goalState;                              // BridgeGoalState, for a goal's event
  bool        goalFinal;                              // the goal's last event
} LoopbackSample;



// -----------------------------------------------------------------------------
//
// LOOPBACK_REPLY - what the loopback calls the answer it gives
//
// ⭐ THE NAME IS THE PLUGIN'S, and this is the whole point of the seam taking
// one. The broker is told "a sub-attribute of this name"; it does not know the
// word, has no branch on it, and a second bridge is free to use another. Here
// it is 'reply', because a transport that goes nowhere has no convention to
// match and the plainest word is the honest one.
//
#define LOOPBACK_REPLY  "reply"



// -----------------------------------------------------------------------------
//
// Goals - what the loopback does with one, per endpoint
//
// "goalMode": { "<endpoint>": "succeed" | "hold" | "progress" | "reject" | "abort" | "stubborn" | "unreachable" | "silent" | "instant" }
//
//   succeed  accepted, one feedback, succeeded, and the result - the goal
//            echoed, as a service's reply is. The default.
//   hold     accepted, and then nothing until it is cancelled - which is how a
//            test gets a goal in flight to cancel
//   progress held, like hold - after one feedback: a goal in flight that has
//            reported progress, for a test to see it (goalFeedback)
//   reject   refused at once. No result, so that event is the final one.
//   abort    accepted, then aborted, with a result
//   stubborn held, like hold - but a cancel cannot be sent, which is how a
//            test gets a failed cancel
//   unreachable  the goal itself cannot be sent - a server that cannot be
//            reached, and so a request that must not write the attribute
//   instant  over with its first and only event: succeeded, with the result -
//            a goal that was never in progress, whose instance is written and
//            removed at once (the broker's goalEndLater)
//   silent   sent, and never answered - not even accepted. The broker gives up
//            after --ddsSyncTimeout and cancels it, which is how a test sees
//            that a goal nobody answered is not left running
//
// The envelope names are the loopback's own, and plain: a transport that goes
// nowhere has no convention to match.
//
#define LOOPBACK_GOAL_STATUS    "status"
#define LOOPBACK_GOAL_FEEDBACK  "feedback"
#define LOOPBACK_GOAL_RESULT    "result"



// -----------------------------------------------------------------------------
//
// loopbackGoalPart - the BridgeGoalPart a payload in this sub-attribute is
//
static int loopbackGoalPart(const char* subAttrName)
{
  if (subAttrName == NULL)                                    return BridgeGoalPartNone;
  if (strcmp(subAttrName, LOOPBACK_GOAL_STATUS)   == 0)       return BridgeGoalPartStatus;
  if (strcmp(subAttrName, LOOPBACK_GOAL_FEEDBACK) == 0)       return BridgeGoalPartFeedback;
  if (strcmp(subAttrName, LOOPBACK_GOAL_RESULT)   == 0)       return BridgeGoalPartResult;

  return BridgeGoalPartNone;
}

typedef struct LoopbackGoalMode
{
  char*  endpoint;
  char*  mode;
} LoopbackGoalMode;

static LoopbackGoalMode  goalModes[LOOPBACK_CHANNELS_MAX];
static int               goalModeCount = 0;

typedef struct LoopbackHeldGoal
{
  char*     endpoint;
  char*     json;
  uint64_t  token;
} LoopbackHeldGoal;

static LoopbackHeldGoal  heldGoals[LOOPBACK_CHANNELS_MAX];
static int               heldGoalCount = 0;
static pthread_mutex_t   heldGoalMutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_t  deliveryThread;
static bool       deliveryRunning = false;

static LoopbackSample  queue[LOOPBACK_CHANNELS_MAX];
static int             queueCount = 0;
static pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;



// -----------------------------------------------------------------------------
//
// loopbackNowMs - a monotonic clock, in milliseconds
//
static int64_t loopbackNowMs(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}



// -----------------------------------------------------------------------------
//
// Reply delays - "replyDelayMs": { "<endpoint>": <ms> } in the bridge config
//
// How late the loopback answers a service, per endpoint, and -1 for never.
//
// ⭐ A TRANSPORT THAT ALWAYS ANSWERS AT ONCE CANNOT TEST A REQUEST THAT WAITS.
// A request that waits for its reply (ddsSync) has two outcomes a test must be
// able to produce on purpose: no answer at all, which has to end in a timeout
// rather than a hang, and an answer that arrives after the requester gave up,
// which must not be written anywhere. A real server produces both by being
// slow or gone; the loopback has to be told to.
//
typedef struct LoopbackReplyDelay
{
  char*  endpoint;
  int    ms;                                          // -1: never answer
} LoopbackReplyDelay;

static LoopbackReplyDelay  replyDelays[LOOPBACK_CHANNELS_MAX];
static int                 replyDelayCount = 0;



// -----------------------------------------------------------------------------
//
// Meta - "meta": { "<endpoint>": "<json object>" } in the bridge config (ABI 6)
//
// What the loopback says ABOUT every payload on that endpoint - a sample, a
// reply, a goal event - the way a real transport hands over its own curiosities
// beside the data. It exists so the broker's side of the meta object can be
// tested without one.
//
typedef struct LoopbackMeta
{
  char*  endpoint;
  char*  meta;
} LoopbackMeta;

static LoopbackMeta  metas[LOOPBACK_CHANNELS_MAX];
static int           metaCount = 0;



// -----------------------------------------------------------------------------
//
// loopbackMeta - the meta configured for this endpoint, or NULL
//
static const char* loopbackMeta(const char* endpoint)
{
  for (int ix = 0; ix < metaCount; ix++)
  {
    if (strcmp(metas[ix].endpoint, endpoint) == 0)
      return metas[ix].meta;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// Request echoes - "echoRequest": { "<endpoint>": "<sub-attribute name>" } (ABI 7)
//
// A reply on that endpoint goes back WITH the request it answers, under that
// sub-attribute, through replyExchangeIn - what a transport that knows its
// requests does (DDS: "request" beside "reply"). The loopback's reply is its
// request, so what was asked is simply the payload again.
//
static LoopbackMeta  echoes[LOOPBACK_CHANNELS_MAX];   // endpoint -> the request's sub-attribute name
static int           echoCount = 0;

static const char* loopbackEchoName(const char* endpoint)
{
  for (int ix = 0; ix < echoCount; ix++)
  {
    if (strcmp(echoes[ix].endpoint, endpoint) == 0)
      return echoes[ix].meta;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// loopbackReplyDelay - how late to answer on this endpoint (0: at once, -1: never)
//
static int loopbackReplyDelay(const char* endpoint)
{
  for (int ix = 0; ix < replyDelayCount; ix++)
  {
    if (strcmp(replyDelays[ix].endpoint, endpoint) == 0)
      return replyDelays[ix].ms;
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// loopbackDelivery - the "network" thread
//
static void* loopbackDelivery(void* vP)
{
  (void) vP;

  while (deliveryRunning == true)
  {
    LoopbackSample sample = { NULL, NULL, NULL, 0, 0, false, 0, false };
    int64_t        now    = loopbackNowMs();

    //
    // The first sample that is DUE, not simply the first one - a reply held
    // back by replyDelayMs must not hold back everything queued behind it.
    //
    pthread_mutex_lock(&queueMutex);
    for (int ix = 0; ix < queueCount; ix++)
    {
      if (queue[ix].dueMs > now)
        continue;

      sample = queue[ix];
      for (int i = ix + 1; i < queueCount; i++)
        queue[i - 1] = queue[i];
      queueCount--;
      break;
    }
    pthread_mutex_unlock(&queueMutex);

    if (sample.endpoint != NULL)
    {
      //
      // A reply goes back QUALIFIED - it belongs to the attribute that was
      // written, but it is not that attribute's value. Guarded on the broker's
      // ABI because a host built before services existed has no such entry
      // point, and reading the member to test it would be reading past the end
      // of the struct it allocated.
      //
      //
      // A reply somebody waits for goes back through replyIn(), carrying the
      // broker's token, so that it reaches its own request and no other. ABI 3
      // AND a non-NULL slot, because a host that is not the broker fills in
      // only what it needs.
      //
      //
      // A goal's event carries the token, the goal's names, where it stands and
      // whether it is the last one - ABI 4 and a non-NULL slot, as for replyIn().
      //
      //
      // ABI 6: with a meta configured for the endpoint, and a host that takes
      // it, every payload goes with it
      //
      const char* meta    = loopbackMeta(sample.endpoint);
      bool        useMeta = (meta != NULL) && (brokerP->abiVersion >= 6);

      if (sample.goal == true)
      {
        if ((brokerP->abiVersion >= 4) && (brokerP->goalEventIn != NULL))
        {
          char goalId[32];
          char goalAlias[64];

          snprintf(goalId,    sizeof(goalId),    "%llu", (unsigned long long) sample.token);
          // urn:goal:<id> - the form the DDS plugin gives a goal, so ?goal=<id> finds it here too
          snprintf(goalAlias, sizeof(goalAlias), "urn:goal:%llu", (unsigned long long) sample.token);

          //
          // ABI 5: say which part of the goal the payload is - the loopback's own
          // names say it, and only the plugin can know them
          //
          if ((useMeta == true) && (brokerP->goalEventMetaIn != NULL))
            brokerP->goalEventMetaIn("loopback", sample.endpoint, sample.token, goalId, goalAlias,
                                     sample.goalState, sample.goalFinal, loopbackGoalPart(sample.subAttrName),
                                     sample.subAttrName, sample.json, meta, 0);
          else if ((brokerP->abiVersion >= 5) && (brokerP->goalEventPartIn != NULL))
            brokerP->goalEventPartIn("loopback", sample.endpoint, sample.token, goalId, goalAlias,
                                     sample.goalState, sample.goalFinal, loopbackGoalPart(sample.subAttrName),
                                     sample.subAttrName, sample.json, 0);
          else
            brokerP->goalEventIn("loopback", sample.endpoint, sample.token, goalId, goalAlias,
                                 sample.goalState, sample.goalFinal, sample.subAttrName, sample.json, 0);
        }
        else
          KT_E("loopback: a goal event on '%s' has nowhere to go - the host predates the action contract", sample.endpoint);
      }
      else if ((sample.subAttrName == NULL) && (useMeta == true) && (brokerP->sampleMetaIn != NULL))
        brokerP->sampleMetaIn("loopback", sample.endpoint, sample.json, meta, 0);
      else if (sample.subAttrName == NULL)
        brokerP->sampleIn("loopback", sample.endpoint, sample.json, 0);
      else if ((loopbackEchoName(sample.endpoint) != NULL) && (brokerP->abiVersion >= 7) && (brokerP->replyExchangeIn != NULL))
        brokerP->replyExchangeIn("loopback", sample.endpoint, sample.token, NULL,
                                 loopbackEchoName(sample.endpoint), sample.json, NULL, 0,
                                 sample.subAttrName, sample.json, meta, 0);
      else if ((useMeta == true) && (brokerP->replyMetaIn != NULL))
        brokerP->replyMetaIn("loopback", sample.endpoint, sample.token, NULL, sample.subAttrName, sample.json, meta, 0);
      else if ((sample.token != 0) && (brokerP->abiVersion >= 3) && (brokerP->replyIn != NULL))
        brokerP->replyIn("loopback", sample.endpoint, sample.token, NULL, sample.subAttrName, sample.json, 0);
      else if ((brokerP->abiVersion >= 2) && (brokerP->sampleQualifiedIn != NULL))
        brokerP->sampleQualifiedIn("loopback", sample.endpoint, NULL, sample.subAttrName, sample.json, 0);
      else
        KT_E("loopback: a reply on '%s' has nowhere to go - the host predates the service contract", sample.endpoint);

      free(sample.endpoint);
      free(sample.json);
    }
    else
      usleep(1000);
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// loopbackChannelLookup - caller holds channelMutex
//
static LoopbackChannel* loopbackChannelLookup(const char* endpoint)
{
  for (int i = 0; i < LOOPBACK_CHANNELS_MAX; i++)
  {
    if ((channels[i].inUse == true) && (strcmp(channels[i].endpoint, endpoint) == 0))
      return &channels[i];
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// loopbackInit -
//
//
// loopbackEmitFile - the file the bridge was given, kept for emitAtStart
//
static char loopbackConfigPath[512];



// -----------------------------------------------------------------------------
//
// loopbackEmitAtStart - queue the samples named in the configuration
//
// The configuration may carry, beside the entity mapping the broker reads,
//
//   "loopback": { "emitAtStart": { "P1": "42", "P2": "{\"x\":1}" } }
//
// and each entry is delivered as though it had arrived on that endpoint - on
// the delivery thread, like any other sample.
//
// This exists because loopback is an instrument, not a transport. Nothing
// publishes on it unless something asks, and testing what the broker does with
// an arriving value should not have to wait for a robot. The parsing is
// deliberately crude - a flat object of string values, found by scanning - so
// that a test bridge does not acquire a JSON dependency of its own.
//
static void loopbackEmitAtStart(void);
static void loopbackReplyDelaysLoad(void);
static void loopbackMetasLoad(void);
static void loopbackEchoesLoad(void);
static void loopbackGoalModesLoad(void);


static int loopbackInit(const char* configFile, const BridgeBroker* _brokerP)
{
  if (configFile != NULL)
  {
    strncpy(loopbackConfigPath, configFile, sizeof(loopbackConfigPath) - 1);
    loopbackConfigPath[sizeof(loopbackConfigPath) - 1] = 0;
  }
  else
    loopbackConfigPath[0] = 0;

  if (_brokerP == NULL)
    return BRIDGE_ERR;

  brokerP = _brokerP;

  memset(channels, 0, sizeof(channels));

  deliveryRunning = true;
  if (pthread_create(&deliveryThread, NULL, loopbackDelivery, NULL) != 0)
  {
    deliveryRunning = false;
    //
    // KT_E directly, not brokerP->logFunction. A plugin resolves the broker's
    // symbols at dlopen, which is how mongoc and corDB log, and logFunction is
    // for forwarding a TRANSPORT LIBRARY's own log sink - a callback handed
    // file, line, function and severity that has to go somewhere.
    //
    KT_E("unable to start the loopback delivery thread");
    return BRIDGE_ERR;
  }

  loopbackReplyDelaysLoad();
  loopbackMetasLoad();
  loopbackEchoesLoad();
  loopbackGoalModesLoad();
  loopbackEmitAtStart();

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// loopbackQueue - put a sample on the delivery thread's queue
//
static bool loopbackQueue(const char* endpoint, const char* json, const char* subAttrName, uint64_t token, int delayMs)
{
  bool queued = false;

  pthread_mutex_lock(&queueMutex);
  if (queueCount < LOOPBACK_CHANNELS_MAX)
  {
    queue[queueCount].endpoint    = strdup(endpoint);
    queue[queueCount].json        = strdup(json);
    queue[queueCount].subAttrName = subAttrName;      // a literal, or NULL - never freed
    queue[queueCount].token       = token;
    queue[queueCount].dueMs       = loopbackNowMs() + delayMs;
    queue[queueCount].goal        = false;              // slots are reused - a goal's event may have been here
    queue[queueCount].goalState   = 0;
    queue[queueCount].goalFinal   = false;
    queueCount++;
    queued = true;
  }
  pthread_mutex_unlock(&queueMutex);

  return queued;
}



// -----------------------------------------------------------------------------
//
// loopbackGoalEvent - queue one event of a goal
//
static bool loopbackGoalEvent(const char* endpoint, uint64_t token, int state, bool final, const char* subAttrName, const char* json, int delayMs)
{
  bool queued = false;

  pthread_mutex_lock(&queueMutex);
  if (queueCount < LOOPBACK_CHANNELS_MAX)
  {
    queue[queueCount].endpoint    = strdup(endpoint);
    queue[queueCount].json        = strdup(json);
    queue[queueCount].subAttrName = subAttrName;      // a literal - never freed
    queue[queueCount].token       = token;
    queue[queueCount].dueMs       = loopbackNowMs() + delayMs;
    queue[queueCount].goal        = true;
    queue[queueCount].goalState   = state;
    queue[queueCount].goalFinal   = final;
    queueCount++;
    queued = true;
  }
  pthread_mutex_unlock(&queueMutex);

  return queued;
}



// -----------------------------------------------------------------------------
//
// loopbackEmitAtStart -
//
static void loopbackConfigPairs(const char* member, void (*pairFunc)(const char* key, const char* value))
{
  if (loopbackConfigPath[0] == 0)
    return;

  FILE* fP = fopen(loopbackConfigPath, "r");
  if (fP == NULL)
    return;

  static char buf[65536];
  size_t      len = fread(buf, 1, sizeof(buf) - 1, fP);
  fclose(fP);
  buf[len] = 0;


  char* emitP = strstr(buf, member);
  if (emitP == NULL)
  {
    return;
  }

  char* p = strchr(emitP, '{');
  if (p == NULL)
    return;
  ++p;

  //
  // "endpoint": <value> pairs until the closing brace. The value is taken
  // verbatim, so a JSON object works as well as a number - it is handed to the
  // broker as the sample's payload and parsed there.
  //
  while (*p != 0 && *p != '}')
  {
    while ((*p != 0) && (*p != '"') && (*p != '}'))
      ++p;
    if (*p != '"')
      break;

    char* keyP = ++p;
    while ((*p != 0) && (*p != '"'))
      ++p;
    if (*p == 0)
      break;
    *p++ = 0;

    while ((*p != 0) && (*p != ':'))
      ++p;
    if (*p == 0)
      break;
    ++p;
    while (isspace((unsigned char) *p))
      ++p;

    char* valP = p;
    int   depth = 0;

    if (*p == '"')
    {
      valP = ++p;
      while ((*p != 0) && (*p != '"'))
        ++p;
    }
    else
    {
      while (*p != 0)
      {
        if ((*p == '{') || (*p == '['))
          ++depth;
        else if ((*p == '}') || (*p == ']'))
        {
          if (depth == 0)
            break;
          --depth;
        }
        else if ((*p == ',') && (depth == 0))
          break;
        ++p;
      }
    }

    char saved = *p;
    *p = 0;
    pairFunc(keyP, valP);
    *p = saved;

    if (saved == '"')
      ++p;
  }
}



// -----------------------------------------------------------------------------
//
// loopbackEmitPair / loopbackEmitAtStart - "emitAtStart": a sample per endpoint
//
static void loopbackEmitPair(const char* endpoint, const char* value)
{
  loopbackQueue(endpoint, value, NULL, 0, 0);
}

static void loopbackEmitAtStart(void)
{
  loopbackConfigPairs("\"emitAtStart\"", loopbackEmitPair);
}



// -----------------------------------------------------------------------------
//
// loopbackDelayPair / loopbackReplyDelaysLoad - "replyDelayMs", see replyDelays
//
static void loopbackDelayPair(const char* endpoint, const char* value)
{
  if (replyDelayCount >= LOOPBACK_CHANNELS_MAX)
    return;

  replyDelays[replyDelayCount].endpoint = strdup(endpoint);
  replyDelays[replyDelayCount].ms       = atoi(value);
  ++replyDelayCount;
}

static void loopbackReplyDelaysLoad(void)
{
  loopbackConfigPairs("\"replyDelayMs\"", loopbackDelayPair);
}



// -----------------------------------------------------------------------------
//
// loopbackMetaPair / loopbackMetasLoad - "meta", see metas
//
static void loopbackMetaPair(const char* endpoint, const char* value)
{
  if (metaCount >= LOOPBACK_CHANNELS_MAX)
    return;

  metas[metaCount].endpoint = strdup(endpoint);
  metas[metaCount].meta     = strdup(value);
  ++metaCount;
}

static void loopbackMetasLoad(void)
{
  loopbackConfigPairs("\"meta\"", loopbackMetaPair);
}



// -----------------------------------------------------------------------------
//
// loopbackEchoPair / loopbackEchoesLoad - "echoRequest", see echoes
//
static void loopbackEchoPair(const char* endpoint, const char* value)
{
  if (echoCount >= LOOPBACK_CHANNELS_MAX)
    return;

  echoes[echoCount].endpoint = strdup(endpoint);
  echoes[echoCount].meta     = strdup(value);
  ++echoCount;
}

static void loopbackEchoesLoad(void)
{
  loopbackConfigPairs("\"echoRequest\"", loopbackEchoPair);
}



// -----------------------------------------------------------------------------
//
// loopbackGoalModePair / loopbackGoalModesLoad - "goalMode", see goalModes
//
static void loopbackGoalModePair(const char* endpoint, const char* value)
{
  if (goalModeCount >= LOOPBACK_CHANNELS_MAX)
    return;

  goalModes[goalModeCount].endpoint = strdup(endpoint);
  goalModes[goalModeCount].mode     = strdup(value);
  ++goalModeCount;
}

static void loopbackGoalModesLoad(void)
{
  loopbackConfigPairs("\"goalMode\"", loopbackGoalModePair);
}

static const char* loopbackGoalMode(const char* endpoint)
{
  for (int ix = 0; ix < goalModeCount; ix++)
  {
    if (strcmp(goalModes[ix].endpoint, endpoint) == 0)
      return goalModes[ix].mode;
  }

  return "succeed";
}



// -----------------------------------------------------------------------------
//
// loopbackClose -
//
// Joins the delivery thread before returning. The broker tears down what
// sampleIn() reaches immediately afterwards, so "stopped" has to mean stopped,
// not asked to stop.
//
static void loopbackClose(void)
{
  if (deliveryRunning == true)
  {
    deliveryRunning = false;
    pthread_join(deliveryThread, NULL);
  }

  pthread_mutex_lock(&queueMutex);
  for (int i = 0; i < queueCount; i++)
  {
    free(queue[i].endpoint);
    free(queue[i].json);
  }
  queueCount = 0;
  pthread_mutex_unlock(&queueMutex);

  pthread_mutex_lock(&channelMutex);
  for (int i = 0; i < LOOPBACK_CHANNELS_MAX; i++)
  {
    if (channels[i].inUse == true)
    {
      free(channels[i].endpoint);
      channels[i].inUse = false;
    }
  }
  pthread_mutex_unlock(&channelMutex);

  for (int i = 0; i < replyDelayCount; i++)
    free(replyDelays[i].endpoint);
  replyDelayCount = 0;

  for (int i = 0; i < metaCount; i++)
  {
    free(metas[i].endpoint);
    free(metas[i].meta);
  }
  metaCount = 0;

  for (int i = 0; i < echoCount; i++)
  {
    free(echoes[i].endpoint);
    free(echoes[i].meta);
  }
  echoCount = 0;

  for (int i = 0; i < goalModeCount; i++)
  {
    free(goalModes[i].endpoint);
    free(goalModes[i].mode);
  }
  goalModeCount = 0;

  pthread_mutex_lock(&heldGoalMutex);
  for (int i = 0; i < heldGoalCount; i++)
  {
    free(heldGoals[i].endpoint);
    free(heldGoals[i].json);
  }
  heldGoalCount = 0;
  pthread_mutex_unlock(&heldGoalMutex);
}



// -----------------------------------------------------------------------------
//
// loopbackChannelAdd -
//
static int loopbackChannelAdd(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction)
{
  if ((endpoint == NULL) || (*endpoint == 0))
    return BRIDGE_BAD_INPUT;

  int rc = BRIDGE_ERR;

  pthread_mutex_lock(&channelMutex);
  if (loopbackChannelLookup(endpoint) != NULL)
    rc = BRIDGE_OK;                                   // already carried - adding twice is not an error
  else
  {
    for (int i = 0; i < LOOPBACK_CHANNELS_MAX; i++)
    {
      if (channels[i].inUse == false)
      {
        channels[i].endpoint  = strdup(endpoint);
        channels[i].direction = direction;
        channels[i].inUse     = true;
        rc = BRIDGE_OK;
        break;
      }
    }
  }
  pthread_mutex_unlock(&channelMutex);

  return rc;
}



// -----------------------------------------------------------------------------
//
// loopbackChannelDel -
//
static int loopbackChannelDel(const char* endpoint)
{
  if (endpoint == NULL)
    return BRIDGE_BAD_INPUT;

  int rc = BRIDGE_NOT_FOUND;

  pthread_mutex_lock(&channelMutex);
  LoopbackChannel* channelP = loopbackChannelLookup(endpoint);
  if (channelP != NULL)
  {
    free(channelP->endpoint);
    channelP->endpoint = NULL;
    channelP->inUse    = false;
    rc = BRIDGE_OK;
  }
  pthread_mutex_unlock(&channelMutex);

  return rc;
}



// -----------------------------------------------------------------------------
//
// loopbackPublish - send a value out, and so receive it again
//
// Called on a BROKER thread, so it queues and returns rather than delivering
// here. See the note on the delivery thread above.
//
static int loopbackPublish(const char* endpoint, const char* json)
{
  if ((endpoint == NULL) || (json == NULL))
    return BRIDGE_BAD_INPUT;

  pthread_mutex_lock(&channelMutex);
  bool known = (loopbackChannelLookup(endpoint) != NULL);
  pthread_mutex_unlock(&channelMutex);

  if (known == false)
    return BRIDGE_NOT_FOUND;

  return (loopbackQueue(endpoint, json, NULL, 0, 0) == true) ? BRIDGE_OK : BRIDGE_ERR;
}



// -----------------------------------------------------------------------------
//
// loopbackServiceInvoke - ask, and be answered by the only peer there is
//
// ⭐ THE ECHO IS THE POINT, not a shortcut. A transport that goes nowhere has
// no application behind it to compute an answer, so it answers with what it was
// asked - exactly as publish() hands back the value it was given. What that
// makes testable is everything on the BROKER's side of an invocation: that
// writing a bound attribute invokes instead of publishing, that the answer
// comes back on a thread the broker did not create, and that it lands in a
// sub-attribute without disturbing the value that asked for it.
//
// Called on a BROKER thread, so it queues and returns - as publish().
//
static int loopbackInvoke(const char* endpoint, const char* json, uint64_t token)
{
  if ((endpoint == NULL) || (json == NULL))
    return BRIDGE_BAD_INPUT;

  pthread_mutex_lock(&channelMutex);
  bool known = (loopbackChannelLookup(endpoint) != NULL);
  pthread_mutex_unlock(&channelMutex);

  if (known == false)
    return BRIDGE_NOT_FOUND;

  //
  // A service configured never to answer still ACCEPTS the request - which is
  // what a real server that has hung does. Refusing it would be a different
  // failure, the one BRIDGE_NOT_FOUND already is.
  //
  int delayMs = loopbackReplyDelay(endpoint);

  if (delayMs < 0)
    return BRIDGE_OK;

  return (loopbackQueue(endpoint, json, LOOPBACK_REPLY, token, delayMs) == true) ? BRIDGE_OK : BRIDGE_ERR;
}



static int loopbackServiceInvoke(const char* endpoint, const char* json)
{
  return loopbackInvoke(endpoint, json, 0);
}



// -----------------------------------------------------------------------------
//
// loopbackServiceInvokeTracked - the same echo, for a request somebody waits for
//
// The token goes back with the reply, through replyIn(). See BridgeDriver.h.
//
static int loopbackServiceInvokeTracked(const char* endpoint, const char* json, uint64_t token)
{
  return loopbackInvoke(endpoint, json, token);
}



// -----------------------------------------------------------------------------
//
// loopbackGoalHold - a goal in flight until cancelled
//
// Kept here, because the cancel is by token and this is where the token meets
// what was asked.
//
static int loopbackGoalHold(const char* endpoint, const char* json, uint64_t token)
{
  int rc = BRIDGE_ERR;

  pthread_mutex_lock(&heldGoalMutex);
  if (heldGoalCount < LOOPBACK_CHANNELS_MAX)
  {
    heldGoals[heldGoalCount].endpoint = strdup(endpoint);
    heldGoals[heldGoalCount].json     = strdup(json);
    heldGoals[heldGoalCount].token    = token;
    ++heldGoalCount;
    rc = BRIDGE_OK;
  }
  pthread_mutex_unlock(&heldGoalMutex);

  return rc;
}



// -----------------------------------------------------------------------------
//
// loopbackActionGoalSend - run a goal, as the endpoint's goalMode says
//
// Called on a BROKER thread, so every event is queued, never delivered here -
// and they are queued in the order a real server would produce them, the
// final one last, which is the plugin's side of the contract.
//
static int loopbackActionGoalSend(const char* endpoint, const char* json, uint64_t token)
{
  if ((endpoint == NULL) || (json == NULL) || (token == 0))
    return BRIDGE_BAD_INPUT;

  pthread_mutex_lock(&channelMutex);
  bool known = (loopbackChannelLookup(endpoint) != NULL);
  pthread_mutex_unlock(&channelMutex);

  if (known == false)
    return BRIDGE_NOT_FOUND;

  const char* mode = loopbackGoalMode(endpoint);

  if (strcmp(mode, "unreachable") == 0)
    return BRIDGE_ERR;

  if (strcmp(mode, "reject") == 0)
  {
    loopbackGoalEvent(endpoint, token, BridgeGoalRejected, true, LOOPBACK_GOAL_STATUS, "{\"code\":\"REJECTED\"}", 0);
    return BRIDGE_OK;
  }

  if (strcmp(mode, "silent") == 0)
    return loopbackGoalHold(endpoint, json, token);

  if (strcmp(mode, "instant") == 0)
  {
    loopbackGoalEvent(endpoint, token, BridgeGoalSucceeded, true, LOOPBACK_GOAL_RESULT, json, 0);
    return BRIDGE_OK;
  }

  loopbackGoalEvent(endpoint, token, BridgeGoalAccepted, false, LOOPBACK_GOAL_STATUS, "{\"code\":\"ACCEPTED\"}", 0);

  if (strcmp(mode, "progress") == 0)
    loopbackGoalEvent(endpoint, token, BridgeGoalExecuting, false, LOOPBACK_GOAL_FEEDBACK, "{\"progress\":50}", 0);

  if ((strcmp(mode, "hold") == 0) || (strcmp(mode, "stubborn") == 0) || (strcmp(mode, "progress") == 0))
    return loopbackGoalHold(endpoint, json, token);

  if (strcmp(mode, "abort") == 0)
  {
    loopbackGoalEvent(endpoint, token, BridgeGoalAborted, false, LOOPBACK_GOAL_STATUS, "{\"code\":\"ABORTED\"}", 0);
    loopbackGoalEvent(endpoint, token, BridgeGoalAborted, true,  LOOPBACK_GOAL_RESULT, json, 0);
    return BRIDGE_OK;
  }

  loopbackGoalEvent(endpoint, token, BridgeGoalExecuting, false, LOOPBACK_GOAL_FEEDBACK, "{\"progress\":50}", 0);
  loopbackGoalEvent(endpoint, token, BridgeGoalSucceeded, false, LOOPBACK_GOAL_STATUS,   "{\"code\":\"SUCCEEDED\"}", 0);
  loopbackGoalEvent(endpoint, token, BridgeGoalSucceeded, true,  LOOPBACK_GOAL_RESULT,   json, 0);

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// loopbackActionGoalCancel - cancel a held goal
//
// Only a held goal is still running here; any other has finished, or was never
// sent, and there is nothing to cancel.
//
static int loopbackActionGoalCancel(const char* endpoint, uint64_t token)
{
  if (strcmp(loopbackGoalMode(endpoint), "stubborn") == 0)
    return BRIDGE_ERR;

  LoopbackHeldGoal held = { NULL, NULL, 0 };

  pthread_mutex_lock(&heldGoalMutex);
  for (int ix = 0; ix < heldGoalCount; ix++)
  {
    if ((heldGoals[ix].token == token) && (strcmp(heldGoals[ix].endpoint, endpoint) == 0))
    {
      held = heldGoals[ix];
      for (int i = ix + 1; i < heldGoalCount; i++)
        heldGoals[i - 1] = heldGoals[i];
      --heldGoalCount;
      break;
    }
  }
  pthread_mutex_unlock(&heldGoalMutex);

  if (held.endpoint == NULL)
    return BRIDGE_NOT_FOUND;

  //
  // As late as the endpoint's replyDelayMs says - a real server takes its time
  // to stop, and a test of what the broker does in the meantime needs it to.
  //
  int delayMs = loopbackReplyDelay(endpoint);

  if (delayMs < 0)
    delayMs = 0;

  loopbackGoalEvent(endpoint, token, BridgeGoalCanceled, false, LOOPBACK_GOAL_STATUS, "{\"code\":\"CANCELED\"}", delayMs);
  loopbackGoalEvent(endpoint, token, BridgeGoalCanceled, true,  LOOPBACK_GOAL_RESULT, held.json, delayMs);

  free(held.endpoint);
  free(held.json);

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// loopbackVersionInfo -
//
static const char* loopbackVersionInfo(void)
{
  return "loopback 0.1.0 (no transport)";
}



// -----------------------------------------------------------------------------
//
// bridgeRegister - the one symbol this .so exports
//
void bridgeRegister(BridgeDriver* driverP)
{
  //
  // What the HOST speaks, read before anything is written - the host owns this
  // struct and allocated it at its own size. Zero means a host from before the
  // handshake, and the safe reading of that is 1. See BridgeDriver.h.
  //
  const int hostAbi = (driverP->abiVersion > 0) ? driverP->abiVersion : 1;

  driverP->alias       = "loopback";
  driverP->version     = "0.1.0";
  driverP->args        = NULL;
  driverP->init        = loopbackInit;
  driverP->close       = loopbackClose;
  driverP->channelAdd  = loopbackChannelAdd;
  driverP->channelDel  = loopbackChannelDel;
  driverP->publish     = loopbackPublish;
  driverP->versionInfo = loopbackVersionInfo;

  //
  // ABI 2, and only where the host has the slot. serverIface stays NULL either
  // way: there is nothing for the loopback to serve TO - it is already both
  // ends of its own wire.
  //
  if (hostAbi >= 2)
    driverP->serviceInvoke = loopbackServiceInvoke;

  if (hostAbi >= 3)
    driverP->serviceInvokeTracked = loopbackServiceInvokeTracked;

  if (hostAbi >= 4)
  {
    driverP->actionGoalSend   = loopbackActionGoalSend;
    driverP->actionGoalCancel = loopbackActionGoalCancel;
  }

  //
  // And now the field is the PLUGIN's, which is what the host reads back.
  //
  driverP->abiVersion = BRIDGE_ABI_VERSION;
}

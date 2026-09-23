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
    LoopbackSample sample = { NULL, NULL, NULL, 0, 0 };
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
      if (sample.subAttrName == NULL)
        brokerP->sampleIn("loopback", sample.endpoint, sample.json, 0);
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
}



// -----------------------------------------------------------------------------
//
// loopbackChannelAdd -
//
static int loopbackChannelAdd(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction)
{
  if ((endpoint == NULL) || (*endpoint == 0))
    return BRIDGE_BAD_INPUT;

  //
  // Actions are a lifecycle - a goal, feedback over time, a result, a cancel -
  // and answering BRIDGE_UNSUPPORTED is the contract's way of saying this
  // transport does not carry one yet.
  //
  if (kind == BridgeChannelAction)
    return BRIDGE_UNSUPPORTED;

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

  //
  // And now the field is the PLUGIN's, which is what the host reads back.
  //
  driverP->abiVersion = BRIDGE_ABI_VERSION;
}

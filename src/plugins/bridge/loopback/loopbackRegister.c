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
// loopbackDelivery - the "network" thread
//
static void* loopbackDelivery(void* vP)
{
  (void) vP;

  while (deliveryRunning == true)
  {
    LoopbackSample sample = { NULL, NULL };

    pthread_mutex_lock(&queueMutex);
    if (queueCount > 0)
    {
      sample = queue[0];
      for (int i = 1; i < queueCount; i++)
        queue[i - 1] = queue[i];
      queueCount--;
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
      if (sample.subAttrName == NULL)
        brokerP->sampleIn("loopback", sample.endpoint, sample.json, 0);
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

  loopbackEmitAtStart();

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// loopbackQueue - put a sample on the delivery thread's queue
//
static bool loopbackQueue(const char* endpoint, const char* json, const char* subAttrName)
{
  bool queued = false;

  pthread_mutex_lock(&queueMutex);
  if (queueCount < LOOPBACK_CHANNELS_MAX)
  {
    queue[queueCount].endpoint    = strdup(endpoint);
    queue[queueCount].json        = strdup(json);
    queue[queueCount].subAttrName = subAttrName;      // a literal, or NULL - never freed
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
static void loopbackEmitAtStart(void)
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


  char* emitP = strstr(buf, "\"emitAtStart\"");
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
    loopbackQueue(keyP, valP, NULL);
    *p = saved;

    if (saved == '"')
      ++p;
  }
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

  return (loopbackQueue(endpoint, json, NULL) == true) ? BRIDGE_OK : BRIDGE_ERR;
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
static int loopbackServiceInvoke(const char* endpoint, const char* json)
{
  if ((endpoint == NULL) || (json == NULL))
    return BRIDGE_BAD_INPUT;

  pthread_mutex_lock(&channelMutex);
  bool known = (loopbackChannelLookup(endpoint) != NULL);
  pthread_mutex_unlock(&channelMutex);

  if (known == false)
    return BRIDGE_NOT_FOUND;

  return (loopbackQueue(endpoint, json, LOOPBACK_REPLY) == true) ? BRIDGE_OK : BRIDGE_ERR;
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

  //
  // And now the field is the PLUGIN's, which is what the host reads back.
  //
  driverP->abiVersion = BRIDGE_ABI_VERSION;
}

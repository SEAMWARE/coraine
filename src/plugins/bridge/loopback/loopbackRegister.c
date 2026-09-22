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
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // malloc, free
#include <string.h>                                   // strcmp, strdup, memset
#include <unistd.h>                                   // usleep

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
  char* endpoint;
  char* json;
} LoopbackSample;

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
      brokerP->sampleIn("loopback", sample.endpoint, sample.json, 0);
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
static int loopbackInit(const char* configFile, const BridgeBroker* _brokerP)
{
  (void) configFile;                                  // loopback has nothing to configure

  if (_brokerP == NULL)
    return BRIDGE_ERR;

  brokerP = _brokerP;

  memset(channels, 0, sizeof(channels));

  deliveryRunning = true;
  if (pthread_create(&deliveryThread, NULL, loopbackDelivery, NULL) != 0)
  {
    deliveryRunning = false;
    brokerP->logFunction(BRIDGE_LOG_ERROR, __FILE__, __LINE__, __FUNCTION__, "unable to start the loopback delivery thread");
    return BRIDGE_ERR;
  }

  return BRIDGE_OK;
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
  // Services and actions are request/response shapes. Loopback carries values,
  // and answering BRIDGE_UNSUPPORTED is the contract's way of saying so.
  //
  if (kind != BridgeChannelTopic)
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

  int rc = BRIDGE_ERR;

  pthread_mutex_lock(&queueMutex);
  if (queueCount < LOOPBACK_CHANNELS_MAX)
  {
    queue[queueCount].endpoint = strdup(endpoint);
    queue[queueCount].json     = strdup(json);
    queueCount++;
    rc = BRIDGE_OK;
  }
  pthread_mutex_unlock(&queueMutex);

  return rc;
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
  driverP->alias       = "loopback";
  driverP->version     = "0.1.0";
  driverP->abiVersion  = BRIDGE_ABI_VERSION;
  driverP->args        = NULL;
  driverP->init        = loopbackInit;
  driverP->close       = loopbackClose;
  driverP->channelAdd  = loopbackChannelAdd;
  driverP->channelDel  = loopbackChannelDel;
  driverP->publish     = loopbackPublish;
  driverP->versionInfo = loopbackVersionInfo;
}

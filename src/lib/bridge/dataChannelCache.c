//
// FILE            dataChannelCache.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The DataChannels - see dataChannelCache.h. A list: a deployment has a handful, and a lookup with none
// (every deployment without DataChannels) is one load of a NULL head.
//
#include <stdlib.h>                                   // calloc, free, realloc
#include <string.h>                                   // strcmp, strdup, strncmp, strlen
#include <stdio.h>                                    // snprintf
#include <pthread.h>                                  // pthread_mutex_*

#include "corLog/corLog.h"                            // COR_T
#include "corBridge/BridgeDriver.h"                   // BridgeDriver

#include "bridge/channelCache.h"                      // CHANNEL_*, channelLookup, bridgeLookup
#include "bridge/recordMap.h"                         // recordMapFree
#include "bridge/dataChannelCache.h"                  // Own interface
#include "coraineTraceLevels.h"                       // CtBridge



static DataChannel*     dataChannelList = NULL;
static pthread_mutex_t  statusMutex     = PTHREAD_MUTEX_INITIALIZER;   // dataChannelStatusSet's retired reasons



// -----------------------------------------------------------------------------
//
// dataChannelLookup -
//
DataChannel* dataChannelLookup(const char* bridgeName, const char* endpoint)
{
  if ((bridgeName == NULL) || (endpoint == NULL))
    return NULL;

  for (DataChannel* dcP = __atomic_load_n(&dataChannelList, __ATOMIC_ACQUIRE); dcP != NULL; dcP = dcP->next)
  {
    if ((strcmp(dcP->endpoint, endpoint) == 0) && (strcmp(dcP->bridgeName, bridgeName) == 0))
      return dcP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// dataChannelLookupById - urn:ngsi-ld:DataChannel:<bridge>:<endpoint>
//
DataChannel* dataChannelLookupById(const char* id, Tenant* tenantP)
{
  static const char prefix[] = "urn:ngsi-ld:DataChannel:";

  if ((id == NULL) || (strncmp(id, prefix, sizeof(prefix) - 1) != 0))
    return NULL;

  const char* rest = &id[sizeof(prefix) - 1];

  for (DataChannel* dcP = __atomic_load_n(&dataChannelList, __ATOMIC_ACQUIRE); dcP != NULL; dcP = dcP->next)
  {
    size_t bridgeLen = strlen(dcP->bridgeName);

    if ((dcP->tenantP == tenantP) && (strncmp(rest, dcP->bridgeName, bridgeLen) == 0) &&
        (rest[bridgeLen] == ':') && (strcmp(&rest[bridgeLen + 1], dcP->endpoint) == 0))
      return dcP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// dataChannelFirst -
//
DataChannel* dataChannelFirst(void)
{
  return __atomic_load_n(&dataChannelList, __ATOMIC_ACQUIRE);
}



// -----------------------------------------------------------------------------
//
// dataChannelCreate -
//
int dataChannelCreate(const char* bridgeName, const char* endpoint, Tenant* tenantP, RecordMap* mapP, char* info)
{
  if ((bridgeName == NULL) || (endpoint == NULL) || (*endpoint == 0) || (mapP == NULL))
    return CHANNEL_BAD_INPUT;

  if ((strchr(endpoint, '\t') != NULL) || (strchr(bridgeName, '\t') != NULL))   // the Channel key's separator (channelCache.c)
    return CHANNEL_BAD_INPUT;

  if ((dataChannelLookup(bridgeName, endpoint) != NULL) || (channelLookup(bridgeName, endpoint) != NULL))
    return CHANNEL_DUP_ENDPOINT;

  DataChannel* dcP = (DataChannel*) calloc(1, sizeof(DataChannel));

  if (dcP == NULL)
    return CHANNEL_ERR;

  dcP->bridgeName = strdup(bridgeName);
  dcP->endpoint   = strdup(endpoint);
  dcP->tenantP    = tenantP;

  if ((dcP->bridgeName == NULL) || (dcP->endpoint == NULL))
  {
    free(dcP->bridgeName);
    free(dcP->endpoint);
    free(dcP);
    return CHANNEL_ERR;
  }

  dcP->mapP = mapP;
  dcP->info = info;

  //
  // A Bridge that is not loaded: the DataChannel is still added - valid configuration for a transport this
  // broker was not started with - and dormant, which says so
  //
  if (bridgeLookup(bridgeName) == NULL)
  {
    char reason[256];
    snprintf(reason, sizeof(reason), "no bridge plugin named '%s' is loaded", bridgeName);
    dcP->status       = ChannelStatusDormant;
    dcP->statusReason = strdup(reason);
  }
  else
    dcP->status = ChannelStatusAvailable;

  //
  // At the end of the list: GET /dataChannels in the order of the file
  //
  DataChannel** lastPP = &dataChannelList;

  while (*lastPP != NULL)
    lastPP = &(*lastPP)->next;

  __atomic_store_n(lastPP, dcP, __ATOMIC_RELEASE);

  COR_T(CtBridge, "DataChannel '%s' on bridge '%s' (%s)", endpoint, bridgeName,
        (dcP->status == ChannelStatusAvailable) ? "available" : "dormant");

  return CHANNEL_OK;
}



// -----------------------------------------------------------------------------
//
// dataChannelStatusSet -
//
// A GET /dataChannels may be rendering the reason being replaced, so the old one is not freed here: it is
// kept and freed with the DataChannel. A status changes a handful of times, not per record.
//
int dataChannelStatusSet(const char* bridgeName, const char* endpoint, ChannelStatus status, const char* reason)
{
  DataChannel* dcP = dataChannelLookup(bridgeName, endpoint);

  if (dcP == NULL)
    return CHANNEL_ERR;

  char* newReason = (reason != NULL) ? strdup(reason) : NULL;

  pthread_mutex_lock(&statusMutex);

  char*  oldReason = dcP->statusReason;
  char** retiredV  = (oldReason != NULL) ? (char**) realloc(dcP->retiredReasonV, (dcP->retiredReasons + 1) * sizeof(char*)) : NULL;

  if (retiredV != NULL)
  {
    retiredV[dcP->retiredReasons++] = oldReason;
    dcP->retiredReasonV             = retiredV;
  }

  if ((oldReason == NULL) || (retiredV != NULL))
    __atomic_store_n(&dcP->statusReason, newReason, __ATOMIC_RELEASE);
  else
    free(newReason);                                  // out of memory: the old reason stays rather than be freed under a reader

  __atomic_store_n(&dcP->status, status, __ATOMIC_RELEASE);

  pthread_mutex_unlock(&statusMutex);

  return CHANNEL_OK;
}



// -----------------------------------------------------------------------------
//
// dataChannelsRelease -
//
void dataChannelsRelease(void)
{
  DataChannel* dcP = __atomic_exchange_n(&dataChannelList, NULL, __ATOMIC_ACQ_REL);

  while (dcP != NULL)
  {
    DataChannel* nextP = dcP->next;

    free(dcP->bridgeName);
    free(dcP->endpoint);
    free(dcP->info);
    free(dcP->statusReason);

    for (int ix = 0; ix < dcP->retiredReasons; ix++)
      free(dcP->retiredReasonV[ix]);
    free(dcP->retiredReasonV);

    recordMapFree(dcP->mapP);
    free(dcP);

    dcP = nextP;
  }
}

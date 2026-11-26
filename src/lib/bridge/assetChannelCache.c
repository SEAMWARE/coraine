//
// FILE            assetChannelCache.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The AssetChannels - see assetChannelCache.h. A list: a deployment has a handful, and a lookup with none
// (every deployment without AssetChannels) is one load of a NULL head.
//
#include <stdlib.h>                                   // calloc, free, realloc
#include <string.h>                                   // strcmp, strdup, strncmp, strlen
#include <stdio.h>                                    // snprintf
#include <pthread.h>                                  // pthread_mutex_*

#include "corLog/corLog.h"                            // COR_T
#include "corBridge/BridgeDriver.h"                   // BridgeDriver
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corAlloc/corAllocStrdup.h"                  // corAllocStrdup
#include "corRest/CorRestState.h"                     // corRest

#include "bridge/channelCache.h"                      // CHANNEL_*, channelLookup, bridgeLookup
#include "bridge/assetMap.h"                          // assetMapFree
#if COR_FEATURE_DATA_CHANNELS
#include "bridge/dataChannelCache.h"                  // dataChannelLookup
#endif
#include "bridge/assetChannelCache.h"                 // Own interface
#include "coraineTraceLevels.h"                       // CtBridge



static AssetChannel*    assetChannelList = NULL;
static pthread_mutex_t  statusMutex     = PTHREAD_MUTEX_INITIALIZER;   // assetChannelStatusSet's retired reasons
static pthread_mutex_t  nodeMutex       = PTHREAD_MUTEX_INITIALIZER;   // every AssetChannel's nodeV and refV - never held with another lock



// -----------------------------------------------------------------------------
//
// assetChannelLookup -
//
AssetChannel* assetChannelLookup(const char* bridgeName, const char* endpoint)
{
  if ((bridgeName == NULL) || (endpoint == NULL))
    return NULL;

  for (AssetChannel* acP = __atomic_load_n(&assetChannelList, __ATOMIC_ACQUIRE); acP != NULL; acP = acP->next)
  {
    if ((strcmp(acP->endpoint, endpoint) == 0) && (strcmp(acP->bridgeName, bridgeName) == 0))
      return acP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// assetChannelLookupById - urn:ngsi-ld:AssetChannel:<bridge>:<endpoint>
//
AssetChannel* assetChannelLookupById(const char* id, Tenant* tenantP)
{
  static const char prefix[] = "urn:ngsi-ld:AssetChannel:";

  if ((id == NULL) || (strncmp(id, prefix, sizeof(prefix) - 1) != 0))
    return NULL;

  const char* rest = &id[sizeof(prefix) - 1];

  for (AssetChannel* acP = __atomic_load_n(&assetChannelList, __ATOMIC_ACQUIRE); acP != NULL; acP = acP->next)
  {
    size_t bridgeLen = strlen(acP->bridgeName);

    if ((acP->tenantP == tenantP) && (strncmp(rest, acP->bridgeName, bridgeLen) == 0) &&
        (rest[bridgeLen] == ':') && (strcmp(&rest[bridgeLen + 1], acP->endpoint) == 0))
      return acP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// assetChannelFirst -
//
AssetChannel* assetChannelFirst(void)
{
  return __atomic_load_n(&assetChannelList, __ATOMIC_ACQUIRE);
}



#if COR_FEATURE_CHANNEL_WRITE_BACK
static int outCounter = 0;                            // AssetChannels with channelDirection both - set at start only



// -----------------------------------------------------------------------------
//
// assetChannelDirectionSet -
//
void assetChannelDirectionSet(AssetChannel* acP, BridgeDirection direction)
{
  if ((acP->direction != BridgeDirectionBoth) && (direction == BridgeDirectionBoth))
    outCounter++;
  else if ((acP->direction == BridgeDirectionBoth) && (direction != BridgeDirectionBoth))
    outCounter--;

  acP->direction = direction;
}



// -----------------------------------------------------------------------------
//
// assetChannelOutCount -
//
int assetChannelOutCount(void)
{
  return outCounter;
}
#endif



// -----------------------------------------------------------------------------
//
// assetChannelCreate -
//
int assetChannelCreate(const char* bridgeName, const char* endpoint, Tenant* tenantP, AssetMap* mapP, char* info)
{
  if ((bridgeName == NULL) || (endpoint == NULL) || (*endpoint == 0) || (mapP == NULL))
    return CHANNEL_BAD_INPUT;

  if ((strchr(endpoint, '\t') != NULL) || (strchr(bridgeName, '\t') != NULL))   // the Channel key's separator (channelCache.c)
    return CHANNEL_BAD_INPUT;

  if ((assetChannelLookup(bridgeName, endpoint) != NULL) || (channelLookup(bridgeName, endpoint) != NULL))
    return CHANNEL_DUP_ENDPOINT;

#if COR_FEATURE_DATA_CHANNELS
  if (dataChannelLookup(bridgeName, endpoint) != NULL)
    return CHANNEL_DUP_ENDPOINT;
#endif

  AssetChannel* acP = (AssetChannel*) calloc(1, sizeof(AssetChannel));

  if (acP == NULL)
    return CHANNEL_ERR;

  acP->bridgeName = strdup(bridgeName);
  acP->endpoint   = strdup(endpoint);
  acP->tenantP    = tenantP;

  if ((acP->bridgeName == NULL) || (acP->endpoint == NULL))
  {
    free(acP->bridgeName);
    free(acP->endpoint);
    free(acP);
    return CHANNEL_ERR;
  }

  acP->mapP = mapP;
  acP->info = info;

  pthread_mutex_init(&acP->modelMutex, NULL);
  pthread_mutex_init(&acP->writeMutex, NULL);

  //
  // A Bridge that is not loaded: the AssetChannel is still added - valid configuration for a transport this
  // broker was not started with - and dormant, which says so
  //
  if (bridgeLookup(bridgeName) == NULL)
  {
    char reason[256];
    snprintf(reason, sizeof(reason), "no bridge plugin named '%s' is loaded", bridgeName);
    acP->status       = ChannelStatusDormant;
    acP->statusReason = strdup(reason);
  }
  else
    acP->status = ChannelStatusAvailable;

  //
  // At the end of the list: GET /assetChannels in the order of the file
  //
  AssetChannel** lastPP = &assetChannelList;

  while (*lastPP != NULL)
    lastPP = &(*lastPP)->next;

  __atomic_store_n(lastPP, acP, __ATOMIC_RELEASE);

  COR_T(CtBridge, "AssetChannel '%s' on bridge '%s' (%s)", endpoint, bridgeName,
        (acP->status == ChannelStatusAvailable) ? "available" : "dormant");

  return CHANNEL_OK;
}



// -----------------------------------------------------------------------------
//
// assetChannelStatusSet -
//
// A GET /assetChannels may be rendering the reason being replaced, so the old one is not freed here: it is
// kept and freed with the AssetChannel. A status changes a handful of times, not per sample.
//
int assetChannelStatusSet(const char* bridgeName, const char* endpoint, ChannelStatus status, const char* reason)
{
  AssetChannel* acP = assetChannelLookup(bridgeName, endpoint);

  if (acP == NULL)
    return CHANNEL_ERR;

  //
  // The bridge reads the source again, but the broker refused its model (two entities of one id): dormant, with
  // the broker's reason, until a model is taken
  //
  if ((status == ChannelStatusAvailable) && (__atomic_load_n(&acP->modelRefused, __ATOMIC_ACQUIRE) == true))
  {
    COR_I("bridge '%s': AssetChannel '%s' - dormant still: its model was refused", bridgeName, endpoint);
    return CHANNEL_OK;
  }

  char* newReason = (reason != NULL) ? strdup(reason) : NULL;

  pthread_mutex_lock(&statusMutex);

  char*  oldReason = acP->statusReason;
  char** retiredV  = (oldReason != NULL) ? (char**) realloc(acP->retiredReasonV, (acP->retiredReasons + 1) * sizeof(char*)) : NULL;

  if (retiredV != NULL)
  {
    retiredV[acP->retiredReasons++] = oldReason;
    acP->retiredReasonV             = retiredV;
  }

  if ((oldReason == NULL) || (retiredV != NULL))
    __atomic_store_n(&acP->statusReason, newReason, __ATOMIC_RELEASE);
  else
    free(newReason);                                  // out of memory: the old reason stays rather than be freed under a reader

  __atomic_store_n(&acP->status, status, __ATOMIC_RELEASE);

  pthread_mutex_unlock(&statusMutex);

  return CHANNEL_OK;
}



#if COR_FEATURE_CHANNEL_CHECKS || COR_FEATURE_CHANNEL_WRITE_BACK
// -----------------------------------------------------------------------------
//
// assetChannelMappedFree -
//
void assetChannelMappedFree(AssetMapped* mappedV, int mapped)
{
  for (int ix = 0; ix < mapped; ix++)
  {
    free(mappedV[ix].entityId);
    free(mappedV[ix].attrName);
    free(mappedV[ix].nodeId);
  }

  free(mappedV);
}



// -----------------------------------------------------------------------------
//
// assetChannelMappedLookup -
//
bool assetChannelMappedLookup(Tenant* tenantP, const char* entityId, const char* attrName, AssetChannel** acPP, const char** nodeIdP, const char** attrNameP)
{
  for (AssetChannel* acP = assetChannelFirst(); acP != NULL; acP = acP->next)
  {
    if (acP->tenantP != tenantP)
      continue;

    pthread_mutex_lock(&acP->modelMutex);

    for (int ix = 0; ix < acP->mapped; ix++)
    {
      if ((strcmp(acP->mappedV[ix].entityId, entityId) == 0) && (strcmp(acP->mappedV[ix].attrName, attrName) == 0))
      {
        *acPP      = acP;
        *nodeIdP   = (acP->mappedV[ix].nodeId != NULL) ? corAllocStrdup(&corRest.kalloc, acP->mappedV[ix].nodeId) : NULL;
        *attrNameP = corAllocStrdup(&corRest.kalloc, acP->mappedV[ix].attrName);

        pthread_mutex_unlock(&acP->modelMutex);
        return true;
      }
    }

    pthread_mutex_unlock(&acP->modelMutex);
  }

  return false;
}
#endif



// -----------------------------------------------------------------------------
//
// nodesFree / refsFree -
//
static void nodesFree(AssetNode* nodeV, int nodes)
{
  for (int ix = 0; ix < nodes; ix++)
  {
    free(nodeV[ix].nodeId);
    free(nodeV[ix].entityId);
  }

  if (nodeV != NULL) free(nodeV);
}

static void refsFree(AssetReference* refV, int refs)
{
  for (int ix = 0; ix < refs; ix++)
  {
    free(refV[ix].target);
    if (refV[ix].entityId != NULL) free(refV[ix].entityId);
  }

  if (refV != NULL) free(refV);
}



// -----------------------------------------------------------------------------
//
// assetChannelNodesKeep -
//
void assetChannelNodesKeep(AssetChannel* acP, AssetNode* nodeV, int nodes, AssetReference* refV, int refs)
{
  AssetNode*      newNodeV = (nodes > 0) ? (AssetNode*)      calloc(nodes, sizeof(AssetNode))      : NULL;
  AssetReference* newRefV  = (refs  > 0) ? (AssetReference*) calloc(refs,  sizeof(AssetReference)) : NULL;

  if (((nodes > 0) && (newNodeV == NULL)) || ((refs > 0) && (newRefV == NULL)))
  {
    if (newNodeV != NULL) free(newNodeV);
    if (newRefV  != NULL) free(newRefV);
    return;
  }

  for (int ix = 0; ix < nodes; ix++)
  {
    newNodeV[ix].nodeId   = strdup(nodeV[ix].nodeId);
    newNodeV[ix].entityId = strdup(nodeV[ix].entityId);
    newNodeV[ix].isEntity = nodeV[ix].isEntity;
  }

  for (int ix = 0; ix < refs; ix++)
  {
    newRefV[ix].target   = strdup(refV[ix].target);
    newRefV[ix].entityId = (refV[ix].entityId != NULL) ? strdup(refV[ix].entityId) : NULL;
    newRefV[ix].isEntity = refV[ix].isEntity;
  }

  pthread_mutex_lock(&nodeMutex);

  AssetNode*      oldNodeV = acP->nodeV;
  int             oldNodes = acP->nodes;
  AssetReference* oldRefV  = acP->refV;
  int             oldRefs  = acP->refs;

  acP->nodeV = newNodeV;
  acP->nodes = nodes;
  acP->refV  = newRefV;
  acP->refs  = refs;

  pthread_mutex_unlock(&nodeMutex);

  nodesFree(oldNodeV, oldNodes);
  refsFree(oldRefV, oldRefs);
}



// -----------------------------------------------------------------------------
//
// nodeLookup - the node of that id in the last model of another AssetChannel of exceptP's tenant; NULL.
// Under nodeMutex
//
static AssetNode* nodeLookup(AssetChannel* exceptP, const char* nodeId)
{
  for (AssetChannel* acP = assetChannelFirst(); acP != NULL; acP = acP->next)
  {
    if ((acP == exceptP) || (acP->tenantP != exceptP->tenantP))
      continue;

    for (int ix = 0; ix < acP->nodes; ix++)
    {
      if (strcmp(acP->nodeV[ix].nodeId, nodeId) == 0)
        return &acP->nodeV[ix];
    }
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// assetChannelNodeLookup -
//
bool assetChannelNodeLookup(AssetChannel* exceptP, const char* nodeId, char** entityIdP, bool* isEntityP)
{
  pthread_mutex_lock(&nodeMutex);

  AssetNode* nodeP = nodeLookup(exceptP, nodeId);

  if (nodeP != NULL)
  {
    *entityIdP = corAllocStrdup(&corRest.kalloc, nodeP->entityId);
    *isEntityP = nodeP->isEntity;
  }

  pthread_mutex_unlock(&nodeMutex);

  return ((nodeP != NULL) && (*entityIdP != NULL));
}



// -----------------------------------------------------------------------------
//
// assetChannelsToRemap -
//
int assetChannelsToRemap(AssetChannel* changedP, AssetChannel*** acVP)
{
  int channels = 0;

  for (AssetChannel* acP = assetChannelFirst(); acP != NULL; acP = acP->next)
    channels++;

  AssetChannel** acV = (AssetChannel**) corAlloc(&corRest.kalloc, channels * sizeof(AssetChannel*));
  int            n   = 0;

  if (acV == NULL)
    return 0;

  pthread_mutex_lock(&nodeMutex);

  for (AssetChannel* acP = assetChannelFirst(); acP != NULL; acP = acP->next)
  {
    if ((acP == changedP) || (acP->tenantP != changedP->tenantP))
      continue;

    for (int ix = 0; ix < acP->refs; ix++)
    {
      AssetReference* refP  = &acP->refV[ix];
      AssetNode*      nodeP = nodeLookup(acP, refP->target);

      if ((nodeP == NULL) && (refP->entityId == NULL))
        continue;

      if ((nodeP != NULL) && (refP->entityId != NULL) && (strcmp(nodeP->entityId, refP->entityId) == 0) && (nodeP->isEntity == refP->isEntity))
        continue;

      acV[n++] = acP;                                 // what the reference became has changed
      break;
    }
  }

  pthread_mutex_unlock(&nodeMutex);

  *acVP = acV;
  return n;
}



// -----------------------------------------------------------------------------
//
// assetChannelsRelease -
//
void assetChannelsRelease(void)
{
  AssetChannel* acP = __atomic_exchange_n(&assetChannelList, NULL, __ATOMIC_ACQ_REL);

  while (acP != NULL)
  {
    AssetChannel* nextP = acP->next;

    free(acP->bridgeName);
    free(acP->endpoint);
    free(acP->info);
    free(acP->statusReason);

    for (int ix = 0; ix < acP->retiredReasons; ix++)
      free(acP->retiredReasonV[ix]);
    free(acP->retiredReasonV);

    assetMapFree(acP->mapP);
    free(acP->model);
    nodesFree(acP->nodeV, acP->nodes);
    refsFree(acP->refV, acP->refs);
#if COR_FEATURE_CHANNEL_CHECKS || COR_FEATURE_CHANNEL_WRITE_BACK
    assetChannelMappedFree(acP->mappedV, acP->mapped);
#endif
    pthread_mutex_destroy(&acP->modelMutex);
    free(acP);

    acP = nextP;
  }
}

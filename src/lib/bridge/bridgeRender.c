//
// FILE            bridgeRender.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf
#include <string.h>                                   // strlen, strcmp

#include "corJson/corJsonParse.h"                       // corJsonParse
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAllocStrdup.h"                  // corAllocStrdup
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeString, corTreeInteger, corTreeBoolean, corTreeChildAdd
#include "corJsonld/corLdCompact.h"                   // corLdCompact
#include "corRest/CorRestState.h"                     // corRest

#include "bridge/Channel.h"                           // Channel, ChannelStatus*, ChannelRetention*
#if COR_FEATURE_BRIDGE_RECORDS
#include "bridge/RecordMap.h"                         // RecordMap
#endif
#include "bridge/channelCache.h"                      // channelCacheFirst
#include "bridge/bridgeGoal.h"                        // bridgeGoalNotifyDefault
#include "bridge/bridgeSampleIn.h"                    // bridgeSamplesDropped
#include "bridge/bridgeRender.h"                      // Own interface



// -----------------------------------------------------------------------------
//
// shortOrSelf - the IRI compacted with the request's @context, or itself
//
static const char* shortOrSelf(CorLdContext* contextP, const char* iri)
{
  const char* compact = (iri != NULL) ? corLdCompact(contextP, iri) : NULL;
  return (compact != NULL) ? compact : iri;
}



// -----------------------------------------------------------------------------
//
// channelIdOf -
//
const char* channelIdOf(Channel* channelP)
{
  if (channelP->id != NULL)
    return channelP->id;

  int   len = 20 + strlen(channelP->bridgeName) + 1 + strlen(channelP->endpoint) + 1;   // "urn:ngsi-ld:Channel:" = 20
  char* id  = (char*) corAlloc(&corRest.kalloc, len);

  snprintf(id, len, "urn:ngsi-ld:Channel:%s:%s", channelP->bridgeName, channelP->endpoint);
  return id;
}



// -----------------------------------------------------------------------------
//
// channelOfTenant -
//
Channel* channelOfTenant(Tenant* tenantP, const char* channelId)
{
  for (Channel* channelP = channelCacheFirst(); channelP != NULL; channelP = channelP->next)
  {
    if ((channelP->tenantP == tenantP) && (strcmp(channelIdOf(channelP), channelId) == 0))
      return channelP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// bridgeIdOf -
//
const char* bridgeIdOf(const char* bridgeName)
{
  int   len = 26 + strlen(bridgeName) + 1;   // "urn:ngsi-ld:ContextBridge:" = 26
  char* id  = (char*) corAlloc(&corRest.kalloc, len);

  snprintf(id, len, "urn:ngsi-ld:ContextBridge:%s", bridgeName);
  return id;
}



// -----------------------------------------------------------------------------
//
// kindName, directionName, retentionName -
//
static const char* kindName(BridgeChannelKind kind)
{
  switch (kind)
  {
  case BridgeChannelTopic:    return "topic";
  case BridgeChannelService:  return "service";
  case BridgeChannelAction:   return "action";
  }

  return "unknown";
}

static const char* directionName(BridgeDirection direction)
{
  switch (direction)
  {
  case BridgeDirectionIn:    return "in";
  case BridgeDirectionOut:   return "out";
  case BridgeDirectionBoth:  return "both";
  }

  return "unknown";
}

static const char* retentionName(ChannelRetention retention)
{
  return (retention == ChannelRetentionRelay) ? "relay" : "mirror";
}



// -----------------------------------------------------------------------------
//
// notificationRender - a default goal endpoint, in the subscription's own shape
//
static void notificationRender(CorNode* bodyP, const char* uri, const char* accept)
{
  CorAlloc* allocP      = corRest.kallocP;
  CorNode* notificationP = corTreeObject(allocP, "notification");
  CorNode* endpointP    = corTreeObject(allocP, "endpoint");

  corTreeChildAdd(endpointP, corTreeString(allocP, "uri", (char*) uri));
  corTreeChildAdd(endpointP, corTreeString(allocP, "accept", (char*) ((accept != NULL) ? accept : "application/json")));
  corTreeChildAdd(notificationP, endpointP);
  corTreeChildAdd(bodyP, notificationP);
}



// -----------------------------------------------------------------------------
//
// channelRender -
//
CorNode* channelRender(Channel* channelP, CorLdContext* contextP)
{
  CorAlloc* allocP  = corRest.kallocP;
  CorNode* bodyP  = corTreeObject(allocP, NULL);
  CorNode* entityP = corTreeObject(allocP, "entity");

  corTreeChildAdd(bodyP, corTreeString(allocP, "id",     (char*) channelIdOf(channelP)));
  corTreeChildAdd(bodyP, corTreeString(allocP, "type",   "Channel"));
  corTreeChildAdd(bodyP, corTreeString(allocP, "bridgeId", (char*) bridgeIdOf(channelP->bridgeName)));
  corTreeChildAdd(bodyP, corTreeString(allocP, "channelTarget", channelP->endpoint));

  //
  // What the Channel was configured with for its transport - the text the plugin was given, parsed back
  // (a copy: the parser writes into its buffer). Absent when it has none.
  //
  if ((channelP->info != NULL) && (corRest.corJsonP != NULL))
  {
    int   len  = strlen(channelP->info);
    char* copy = corAlloc(allocP, len + 1);

    if (copy != NULL)
    {
      memcpy(copy, channelP->info, len + 1);

      CorNode* infoP = corJsonParse(corRest.corJsonP, copy);
      if (infoP != NULL)
      {
        infoP->name = (char*) "channelInfo";
        corTreeChildAdd(bodyP, infoP);
      }
    }
  }
  corTreeChildAdd(bodyP, corTreeString(allocP, "channelKind", (char*) kindName(channelP->kind)));
  corTreeChildAdd(bodyP, corTreeString(allocP, "channelDirection", (char*) directionName(channelP->direction)));
  corTreeChildAdd(bodyP, corTreeString(allocP, "retention", (char*) retentionName(channelP->retention)));

  if (channelP->recordMapP == NULL)
  {
    corTreeChildAdd(entityP, corTreeString(allocP, "id", channelP->entityId));
    corTreeChildAdd(entityP, corTreeString(allocP, "type", (char*) shortOrSelf(contextP, channelP->entityType)));
    corTreeChildAdd(bodyP, entityP);

    corTreeChildAdd(bodyP, corTreeString(allocP, "entityAttribute", (char*) shortOrSelf(contextP, channelP->attrName)));
  }
#if COR_FEATURE_BRIDGE_RECORDS
  else
  {
    //
    // A record Channel names no entity: what it shows is its mapping, as the file wrote it
    //
    const char* text = ((RecordMap*) channelP->recordMapP)->text;

    if ((text != NULL) && (corRest.corJsonP != NULL))
    {
      char*    copy     = corAllocStrdup(allocP, text);
      CorNode* recordsP = (copy != NULL) ? corJsonParse(corRest.corJsonP, copy) : NULL;

      if (recordsP != NULL)
      {
        recordsP->name = (char*) "recordEntities";
        corTreeChildAdd(bodyP, recordsP);
      }
    }
  }
#endif

  ChannelStatus status = __atomic_load_n(&channelP->status, __ATOMIC_ACQUIRE);       // channelStatusSet, on a plugin thread
  char*         reason = __atomic_load_n(&channelP->statusReason, __ATOMIC_ACQUIRE);

  corTreeChildAdd(bodyP, corTreeString(allocP, "status", (status == ChannelStatusAvailable) ? "available" : "dormant"));

  if (reason != NULL)
    corTreeChildAdd(bodyP, corTreeString(allocP, "statusReason", reason));

  //
  // endpointDiscovered only once the transport said so (endpointDiscoveredIn): absent means not
  // reported - yet, or by a transport that does not discover - never a false that would read as
  // "looked, and it is not there".
  //
  if (__atomic_load_n(&channelP->endpointDiscovered, __ATOMIC_ACQUIRE) == true)
    corTreeChildAdd(bodyP, corTreeBoolean(allocP, "endpointDiscovered", true));

  corTreeChildAdd(bodyP, corTreeInteger(allocP, "samplesIn",         (long long) __atomic_load_n(&channelP->samplesIn,         __ATOMIC_RELAXED)));
  corTreeChildAdd(bodyP, corTreeInteger(allocP, "samplesOut",        (long long) __atomic_load_n(&channelP->samplesOut,        __ATOMIC_RELAXED)));

  if (channelP->kind == BridgeChannelAction)   // only an action carries goals
  {
    corTreeChildAdd(bodyP, corTreeInteger(allocP, "goalsSent",       (long long) __atomic_load_n(&channelP->goalsSent,       __ATOMIC_RELAXED)));
    corTreeChildAdd(bodyP, corTreeInteger(allocP, "goalCancelsSent", (long long) __atomic_load_n(&channelP->goalCancelsSent, __ATOMIC_RELAXED)));
  }

  corTreeChildAdd(bodyP, corTreeInteger(allocP, "requestsWaiting",   (long long) __atomic_load_n(&channelP->requestsWaiting,   __ATOMIC_RELAXED)));
  corTreeChildAdd(bodyP, corTreeInteger(allocP, "requestsNotWaited", (long long) __atomic_load_n(&channelP->requestsNotWaited, __ATOMIC_RELAXED)));

  if (channelP->notifyUri != NULL)
    notificationRender(bodyP, channelP->notifyUri, channelP->notifyAccept);

  return bodyP;
}



// -----------------------------------------------------------------------------
//
// bridgeRender -
//
CorNode* bridgeRender(const char* bridgeName, bool loaded)
{
  CorAlloc* allocP = corRest.kallocP;
  CorNode* bodyP = corTreeObject(allocP, NULL);

  corTreeChildAdd(bodyP, corTreeString(allocP, "id", (char*) bridgeIdOf(bridgeName)));
  corTreeChildAdd(bodyP, corTreeString(allocP, "type", "ContextBridge"));
  corTreeChildAdd(bodyP, corTreeString(allocP, "plugin", (char*) bridgeName));
  corTreeChildAdd(bodyP, corTreeString(allocP, "status", loaded ? "available" : "unavailable"));
  corTreeChildAdd(bodyP, corTreeInteger(allocP, "samplesDropped", (long long) bridgeSamplesDropped(bridgeName)));

  if (loaded == false)
  {
    int   len    = strlen(bridgeName) + 64;
    char* reason = (char*) corAlloc(&corRest.kalloc, len);

    snprintf(reason, len, "plugin '%s' not loaded - not named on --bridges", bridgeName);
    corTreeChildAdd(bodyP, corTreeString(allocP, "statusReason", reason));
  }

  const char* uri    = NULL;
  const char* accept = NULL;

  if (bridgeGoalNotifyDefault(bridgeName, &uri, &accept) == true)
    notificationRender(bodyP, uri, accept);

  return bodyP;
}

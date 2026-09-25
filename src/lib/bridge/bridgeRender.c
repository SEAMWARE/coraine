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

#include "kalloc/kaAlloc.h"                           // kaAlloc
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjObject, kjString, kjChildAdd
#include "corJsonld/corLdCompact.h"                   // corLdCompact
#include "corRest/CorRestState.h"                     // corRest

#include "bridge/Channel.h"                           // Channel, ChannelStatus*, ChannelRetention*
#include "bridge/channelCache.h"                      // channelCacheFirst
#include "bridge/bridgeGoal.h"                        // bridgeGoalNotifyDefault
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
  char* id  = (char*) kaAlloc(&corRest.kalloc, len);

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
  char* id  = (char*) kaAlloc(&corRest.kalloc, len);

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
static void notificationRender(KjNode* bodyP, const char* uri, const char* accept)
{
  Kjson*  kjsonP        = corRest.kjsonP;
  KjNode* notificationP = kjObject(kjsonP, "notification");
  KjNode* endpointP     = kjObject(kjsonP, "endpoint");

  kjChildAdd(endpointP, kjString(kjsonP, "uri",    (char*) uri));
  kjChildAdd(endpointP, kjString(kjsonP, "accept", (char*) ((accept != NULL) ? accept : "application/json")));
  kjChildAdd(notificationP, endpointP);
  kjChildAdd(bodyP, notificationP);
}



// -----------------------------------------------------------------------------
//
// channelRender -
//
KjNode* channelRender(Channel* channelP, CorLdContext* contextP)
{
  Kjson*  kjsonP  = corRest.kjsonP;
  KjNode* bodyP   = kjObject(kjsonP, NULL);
  KjNode* entityP = kjObject(kjsonP, "entity");

  kjChildAdd(bodyP, kjString(kjsonP, "id",               (char*) channelIdOf(channelP)));
  kjChildAdd(bodyP, kjString(kjsonP, "type",             "Channel"));
  kjChildAdd(bodyP, kjString(kjsonP, "bridgeId",         (char*) bridgeIdOf(channelP->bridgeName)));
  kjChildAdd(bodyP, kjString(kjsonP, "channelTarget",    channelP->endpoint));
  kjChildAdd(bodyP, kjString(kjsonP, "channelKind",      (char*) kindName(channelP->kind)));
  kjChildAdd(bodyP, kjString(kjsonP, "channelDirection", (char*) directionName(channelP->direction)));
  kjChildAdd(bodyP, kjString(kjsonP, "retention",        (char*) retentionName(channelP->retention)));

  kjChildAdd(entityP, kjString(kjsonP, "id",   channelP->entityId));
  kjChildAdd(entityP, kjString(kjsonP, "type", (char*) shortOrSelf(contextP, channelP->entityType)));
  kjChildAdd(bodyP, entityP);

  kjChildAdd(bodyP, kjString(kjsonP, "entityAttribute",  (char*) shortOrSelf(contextP, channelP->attrName)));
  kjChildAdd(bodyP, kjString(kjsonP, "status",           (channelP->status == ChannelStatusAvailable) ? "available" : "dormant"));

  if (channelP->statusReason != NULL)
    kjChildAdd(bodyP, kjString(kjsonP, "statusReason", channelP->statusReason));

  if (channelP->notifyUri != NULL)
    notificationRender(bodyP, channelP->notifyUri, channelP->notifyAccept);

  return bodyP;
}



// -----------------------------------------------------------------------------
//
// bridgeRender -
//
KjNode* bridgeRender(const char* bridgeName, bool loaded)
{
  Kjson*  kjsonP = corRest.kjsonP;
  KjNode* bodyP  = kjObject(kjsonP, NULL);

  kjChildAdd(bodyP, kjString(kjsonP, "id",     (char*) bridgeIdOf(bridgeName)));
  kjChildAdd(bodyP, kjString(kjsonP, "type",   "ContextBridge"));
  kjChildAdd(bodyP, kjString(kjsonP, "plugin", (char*) bridgeName));
  kjChildAdd(bodyP, kjString(kjsonP, "status", loaded ? "available" : "unavailable"));

  if (loaded == false)
  {
    int   len    = strlen(bridgeName) + 64;
    char* reason = (char*) kaAlloc(&corRest.kalloc, len);

    snprintf(reason, len, "plugin '%s' not loaded - not named on --bridges", bridgeName);
    kjChildAdd(bodyP, kjString(kjsonP, "statusReason", reason));
  }

  const char* uri    = NULL;
  const char* accept = NULL;

  if (bridgeGoalNotifyDefault(bridgeName, &uri, &accept) == true)
    notificationRender(bodyP, uri, accept);

  return bodyP;
}

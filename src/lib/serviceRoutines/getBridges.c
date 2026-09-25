//
// FILE            getBridges.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/bridges - the ContextBridges
//
// Every loaded bridge plugin, available - and every Bridge a Channel names
// whose plugin the broker was not started with, unavailable: a configuration
// that is valid and cannot be served is something to see with a GET, not to
// find by noticing that nothing arrives (bridge-channels.md 3.3a).
//
// Bridges are the broker's, not a tenant's.
//
#include <stdbool.h>                                  // bool
#include <string.h>                                   // strcmp

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjArray, kjChildAdd
#include "corRest/CorRestState.h"                     // corRest
#include "corNgsild/corNgsild.h"                      // corNgsild
#include "corBridge/BridgeDriver.h"                   // bridges, bridgeCount

#include "bridge/Channel.h"                           // Channel
#include "bridge/channelCache.h"                      // channelCacheFirst
#include "bridge/bridgeRender.h"                      // bridgeRender
#include "serviceRoutines/getBridges.h"               // Own interface



// -----------------------------------------------------------------------------
//
// bridgeLoaded - is a plugin of this name loaded?
//
bool bridgeLoaded(const char* bridgeName)
{
  for (int ix = 0; ix < bridgeCount; ix++)
  {
    if (strcmp(bridges[ix].alias, bridgeName) == 0)
      return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// unloadedNamedEarlier - was this unloaded Bridge already listed, by an earlier Channel?
//
static bool unloadedNamedEarlier(Channel* channelP)
{
  for (Channel* earlierP = channelCacheFirst(); earlierP != channelP; earlierP = earlierP->next)
  {
    if (strcmp(earlierP->bridgeName, channelP->bridgeName) == 0)
      return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// getBridges -
//
bool getBridges(void)
{
  KjNode* arrayP = kjArray(corRest.kjsonP, NULL);

  // Not an Entity - ldEntityToApi would read the "entity" member as an
  // Attribute keyed by datasetId and render it as an array of instances
  corNgsild.rawResponse = true;

  for (int ix = 0; ix < bridgeCount; ix++)
    kjChildAdd(arrayP, bridgeRender(bridges[ix].alias, true));

  for (Channel* channelP = channelCacheFirst(); channelP != NULL; channelP = channelP->next)
  {
    if ((bridgeLoaded(channelP->bridgeName) == false) && (unloadedNamedEarlier(channelP) == false))
      kjChildAdd(arrayP, bridgeRender(channelP->bridgeName, false));
  }

  corRest.out.responseTree = arrayP;
  return true;
}

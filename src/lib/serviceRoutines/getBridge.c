//
// FILE            getBridge.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/bridges/{bridgeId} - one ContextBridge
//
#include <stdbool.h>                                  // bool
#include <string.h>                                   // strcmp

#include "corRest/CorRestState.h"                     // corRest
#include "corNgsild/corNgsild.h"                      // ldError, LD_ERROR_*
#include "corBridge/BridgeDriver.h"                   // bridges, bridgeCount

#include "bridge/Channel.h"                           // Channel
#include "bridge/channelCache.h"                      // channelCacheFirst
#include "bridge/bridgeRender.h"                      // bridgeRender, bridgeIdOf
#include "serviceRoutines/getBridges.h"               // bridgeLoaded
#include "serviceRoutines/getBridge.h"                // Own interface



// -----------------------------------------------------------------------------
//
// getBridge -
//
bool getBridge(void)
{
  const char* bridgeId = corRest.in.wildcard[0];

  // Not an Entity - ldEntityToApi would read the "entity" member as an
  // Attribute keyed by datasetId and render it as an array of instances
  corNgsild.rawResponse = true;

  for (int ix = 0; ix < bridgeCount; ix++)
  {
    if (strcmp(bridgeIdOf(bridges[ix].alias), bridgeId) == 0)
    {
      corRest.out.responseTree = bridgeRender(bridges[ix].alias, true);
      return true;
    }
  }

  for (Channel* channelP = channelCacheFirst(); channelP != NULL; channelP = channelP->next)
  {
    if (strcmp(bridgeIdOf(channelP->bridgeName), bridgeId) == 0)
    {
      corRest.out.responseTree = bridgeRender(channelP->bridgeName, bridgeLoaded(channelP->bridgeName));
      return true;
    }
  }

  ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "bridge '%s' not found", bridgeId);
  return true;
}

//
// FILE            getChannels.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/channels - the Channels of the request's tenant
//
// Proposed to ETSI with the ContextBridge and the Channel: the path the
// proposal gives them. Today every Channel comes from the configuration file
// (bridge-channels.md 3.6) - they are listed as loaded, read-only.
//
#include <stddef.h>                                   // NULL

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjArray, kjChildAdd
#include "corRest/CorRestState.h"                     // corRest
#include "corNgsild/corNgsild.h"                      // corNgsild, ldContextResolve

#include "db/Tenant.h"                                // Tenant
#include "bridge/Channel.h"                           // Channel
#include "bridge/channelCache.h"                      // channelCacheFirst
#include "bridge/bridgeRender.h"                      // channelRender
#include "serviceRoutines/getChannels.h"              // Own interface



// -----------------------------------------------------------------------------
//
// getChannels -
//
bool getChannels(void)
{
  Tenant* tenantP = (Tenant*) corNgsild.tenantP;
  KjNode* arrayP  = kjArray(corRest.kjsonP, NULL);

  // Not an Entity - ldEntityToApi would read the "entity" member as an
  // Attribute keyed by datasetId and render it as an array of instances
  corNgsild.rawResponse = true;

  ldContextResolve();

  for (Channel* channelP = channelCacheFirst(); channelP != NULL; channelP = channelP->next)
  {
    if (channelP->tenantP != tenantP)
      continue;

    kjChildAdd(arrayP, channelRender(channelP, corNgsild.contextP));
  }

  corRest.out.responseTree = arrayP;
  return true;
}

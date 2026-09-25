//
// FILE            getChannel.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/channels/{channelId} - one Channel of the request's tenant
//
#include <stddef.h>                                   // NULL
#include <string.h>                                   // strcmp

#include "corRest/CorRestState.h"                     // corRest
#include "corNgsild/corNgsild.h"                      // corNgsild, ldError, ldContextResolve, LD_ERROR_*

#include "db/Tenant.h"                                // Tenant
#include "bridge/Channel.h"                           // Channel
#include "bridge/channelCache.h"                      // channelCacheFirst
#include "bridge/bridgeRender.h"                      // channelRender, channelOfTenant
#include "serviceRoutines/getChannel.h"               // Own interface



// -----------------------------------------------------------------------------
//
// getChannel -
//
bool getChannel(void)
{
  const char* channelId = corRest.in.wildcard[0];
  Tenant*     tenantP   = (Tenant*) corNgsild.tenantP;

  // Not an Entity - ldEntityToApi would read the "entity" member as an
  // Attribute keyed by datasetId and render it as an array of instances
  corNgsild.rawResponse = true;

  Channel* channelP = channelOfTenant(tenantP, channelId);

  if (channelP != NULL)
  {
    ldContextResolve();
    corRest.out.responseTree = channelRender(channelP, corNgsild.contextP);
    return true;
  }

  ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "channel '%s' not found", channelId);
  return true;
}

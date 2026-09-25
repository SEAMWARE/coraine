//
// FILE            channelGoalCommon.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                   // NULL

#include "corRest/CorRestState.h"                     // corRest
#include "corNgsild/corNgsild.h"                      // corNgsild, ldError, LD_ERROR_*
#include "corBridge/BridgeDriver.h"                   // BridgeChannelAction

#include "db/Tenant.h"                                // Tenant
#include "bridge/Channel.h"                           // Channel
#include "bridge/bridgeRender.h"                      // channelOfTenant
#include "serviceRoutines/channelGoalCommon.h"        // Own interface



// -----------------------------------------------------------------------------
//
// actionChannelOfRequest -
//
Channel* actionChannelOfRequest(void)
{
  const char* channelId = corRest.in.wildcard[0];
  Channel*    channelP  = channelOfTenant((Tenant*) corNgsild.tenantP, channelId);

  if (channelP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "channel '%s' not found", channelId);
    return NULL;
  }

  if (channelP->kind != BridgeChannelAction)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Not an Action Channel",
            "channel '%s' carries no goals - a goal goes to a Channel whose channelKind is action", channelId);
    return NULL;
  }

  return channelP;
}

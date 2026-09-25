//
// FILE            deleteChannelGoal.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// DELETE /ngsi-ld/v1/channels/{channelId}/goals/{goalId} - cancel a goal
//
// The phase-1 cancel, by the transport's goal id: 202 when the cancel was sent
// (the goal ends when the transport says so, and its instance with it), 503
// when it could not be, and nothing is deleted.
//
#include <stddef.h>                                   // NULL

#include "corRest/CorRestState.h"                     // corRest
#include "corNgsild/corNgsild.h"                      // corNgsild, ldError, LD_ERROR_*
#include "corBridge/BridgeDriver.h"                   // BRIDGE_OK, BRIDGE_UNSUPPORTED

#include "db/Tenant.h"                                // Tenant
#include "bridge/Channel.h"                           // Channel
#include "bridge/bridgeGoal.h"                        // bridgeGoalAliasOf, bridgeGoalCancel
#include "serviceRoutines/channelGoalCommon.h"        // actionChannelOfRequest
#include "serviceRoutines/deleteChannelGoal.h"        // Own interface



// -----------------------------------------------------------------------------
//
// deleteChannelGoal -
//
bool deleteChannelGoal(void)
{
  Channel* channelP = actionChannelOfRequest();

  if (channelP == NULL)
    return true;

  const char* goalId = corRest.in.wildcard[1];
  char*       alias  = bridgeGoalAliasOf(channelP, goalId);
  int         rc     = 0;

  if ((alias == NULL) || (bridgeGoalCancel((Tenant*) corNgsild.tenantP, channelP->entityId, channelP->attrName, alias, &rc) == false))
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "goal '%s' is not in progress on this channel", goalId);
    return true;
  }

  if (rc == BRIDGE_OK)
    corRest.out.httpStatusCode = 202;
  else if (rc == BRIDGE_UNSUPPORTED)
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Operation Not Supported", "the bridge carrying this channel cannot cancel a goal");
  else
    ldError(503, LD_ERROR_INTERNAL_ERROR, "Service Unavailable", "the cancellation of goal '%s' could not be sent (%d) - nothing was cancelled", goalId, rc);

  return true;
}

//
// FILE            getChannelGoal.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/channels/{channelId}/goals/{goalId} - poll a goal
//
// {goalId} is the transport's own id - the one POST put in Location. 404 once
// the goal has ended (as in phase 1: its end is in TRoE and the notifications).
//
#include <stddef.h>                                   // NULL

#include "kjson/KjNode.h"                             // KjNode
#include "corRest/CorRestState.h"                     // corRest
#include "corNgsild/corNgsild.h"                      // corNgsild, ldError, LD_ERROR_*

#include "bridge/Channel.h"                           // Channel
#include "bridge/bridgeGoal.h"                        // bridgeGoalRender
#include "serviceRoutines/channelGoalCommon.h"        // actionChannelOfRequest
#include "serviceRoutines/getChannelGoal.h"           // Own interface



// -----------------------------------------------------------------------------
//
// getChannelGoal -
//
bool getChannelGoal(void)
{
  corNgsild.rawResponse = true;   // not an Entity

  Channel* channelP = actionChannelOfRequest();

  if (channelP == NULL)
    return true;

  const char* goalId = corRest.in.wildcard[1];
  KjNode*     goalP  = bridgeGoalRender(channelP, goalId);

  if (goalP == NULL)
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "goal '%s' is not in progress on this channel", goalId);
  else
    corRest.out.responseTree = goalP;

  return true;
}

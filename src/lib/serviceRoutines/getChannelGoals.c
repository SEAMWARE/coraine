//
// FILE            getChannelGoals.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/channels/{channelId}/goals - an action Channel's goals in progress
//
// Only while in progress, as in phase 1: a goal that has ended is gone from
// here - its end is in TRoE and in the notifications.
//
#include <stddef.h>                                   // NULL

#include "corRest/CorRestState.h"                     // corRest
#include "corNgsild/corNgsild.h"                      // corNgsild

#include "bridge/Channel.h"                           // Channel
#include "bridge/bridgeGoal.h"                        // bridgeGoalsRender
#include "serviceRoutines/channelGoalCommon.h"        // actionChannelOfRequest
#include "serviceRoutines/getChannelGoals.h"          // Own interface



// -----------------------------------------------------------------------------
//
// getChannelGoals -
//
bool getChannelGoals(void)
{
  corNgsild.rawResponse = true;   // not Entities

  Channel* channelP = actionChannelOfRequest();

  if (channelP != NULL)
    corRest.out.responseTree = bridgeGoalsRender(channelP);

  return true;
}

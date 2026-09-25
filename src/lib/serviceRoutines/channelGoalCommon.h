//
// FILE            channelGoalCommon.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#ifndef CHANNEL_GOAL_COMMON_H
#define CHANNEL_GOAL_COMMON_H

#include "bridge/Channel.h"                       // Channel



// -----------------------------------------------------------------------------
//
// actionChannelOfRequest - the request tenant's ACTION Channel, from the URL's first wildcard
//
// NULL with the error set: 404 for no such Channel, 400 for a Channel that
// carries no goals (a topic, a service).
//
extern Channel* actionChannelOfRequest(void);

#endif  // CHANNEL_GOAL_COMMON_H

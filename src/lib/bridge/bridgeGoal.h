#ifndef SRC_LIB_BRIDGE_BRIDGEGOAL_H_
#define SRC_LIB_BRIDGE_BRIDGEGOAL_H_

//
// FILE            bridgeGoal.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Goals - the broker's side of an action Channel, seam ABI 4
//
// Writing an attribute bound to an action Channel SENDS A GOAL, with the value
// written as the goal's request. What then happens to the goal comes back as
// events (BridgeBroker.h, goalEventIn), each carrying the token the broker gave
// the goal when it sent it, and lands in the goal's own instance of that same
// attribute: the instance whose datasetId is the alias the plugin gave the goal.
//
//   the first event   creates the instance, its value the request, and puts the
//                     event's payload in the sub-attribute the plugin named
//   each later one    writes its sub-attribute into that instance
//   the final one     is written like the others, and then the instance goes -
//                     that goal's, and nothing else: the attribute, the value
//                     that was last asked and every other goal's instance stay
//
// Several goals on one attribute are several instances, side by side.
//
// A goal is cancelled by deleting its instance - DELETE of the attribute with
// ?datasetId=<the goal's alias> - and DDS goes first, as for everything the
// broker ASKS the DDS side to do: the cancel is sent, a cancel that cannot be
// sent fails the request, and one that was sent is answered 202. The instance
// goes when the goal ends, as it always does.
//
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // int64_t, uint64_t

#include "db/Tenant.h"                                // Tenant
#include "bridge/Channel.h"                           // Channel



// -----------------------------------------------------------------------------
//
// bridgeGoalSend - send a value written to an action Channel as a goal
//
// Called on a broker thread. Registers the goal under a fresh token BEFORE the
// plugin sees it, because the plugin may report on the goal before it returns.
//
// Always sent BEFORE its request's write (DDS first), so it is HELD: its events
// wait until bridgeGoalRelease(*tokenP) says the write is done.
//
// @param tokenP  the goal's token
//
// @return the plugin's answer: BRIDGE_OK, or why the goal did not go
//
extern int bridgeGoalSend(Channel* channelP, const char* json, uint64_t* tokenP);



// -----------------------------------------------------------------------------
//
// bridgeGoalRelease - the request that sent a held goal has written: events may land
//
extern void bridgeGoalRelease(uint64_t token);



// -----------------------------------------------------------------------------
//
// bridgeGoalEventIn - the broker's side of BridgeBroker::goalEventIn
//
extern int bridgeGoalEventIn(const char* bridgeName,
                             const char* endpoint,
                             uint64_t    token,
                             const char* goalId,
                             const char* goalAlias,
                             int         state,
                             bool        final,
                             const char* subAttrName,
                             const char* json,
                             int64_t     publishTime);



// -----------------------------------------------------------------------------
//
// bridgeGoalCancel - is this the instance of a goal in flight? Then cancel it
//
// For DELETE /entities/{id}/attrs/{attr}?datasetId=X, instead of deleting.
//
// @return false when X is not a goal in flight on an action Channel for that
//         attribute. true when it was, with *rcP holding what the plugin
//         answered.
//
extern bool bridgeGoalCancel(Tenant* tenantP, const char* entityId, const char* attrName, const char* datasetId, int* rcP);

#endif  // SRC_LIB_BRIDGE_BRIDGEGOAL_H_

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
#include "kjson/KjNode.h"                             // KjNode
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
// @param endpoint  where the goal's events are to be notified, or NULL - the
//                  "endpoint" sub-Attribute of the request. The broker then
//                  holds a subscription of its own for that endpoint to THIS
//                  goal's instance, made on its first event and gone after its
//                  last. Only in the broker's memory: never stored, never
//                  listed - it lives exactly as long as the goal.
// @param tokenP    the goal's token
//
// @return the plugin's answer: BRIDGE_OK, or why the goal did not go
//
extern int bridgeGoalSend(Channel* channelP, const char* json, const char* endpoint, uint64_t* tokenP);



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

// -----------------------------------------------------------------------------
//
// bridgeGoalEventPartIn - the broker's side of BridgeBroker::goalEventPartIn (ABI 5)
//
// bridgeGoalEventIn, and the part the payload is (BridgeGoalPart): the latest
// feedback and the result are kept for the goal resource.
//
extern int bridgeGoalEventPartIn(const char* bridgeName,
                                 const char* endpoint,
                                 uint64_t    token,
                                 const char* goalId,
                                 const char* goalAlias,
                                 int         state,
                                 bool        final,
                                 int         part,
                                 const char* subAttrName,
                                 const char* json,
                                 int64_t     publishTime);



// -----------------------------------------------------------------------------
//
// bridgeGoalAwaitAnswer - wait for a goal's FIRST event: accepted, or not
//
// For POST /channels/{id}/goals, which answers with the transport's goal id -
// so it waits for it, and a refused goal is a proper error instead of a 201
// that later fails (KZ: "better error handling if we wait"). Finds an answer
// that came before the wait began. false: none within timeoutMs.
//
extern bool bridgeGoalAwaitAnswer(uint64_t token, int timeoutMs, int* stateP, char* goalIdBuf, int goalIdBufSize);



// -----------------------------------------------------------------------------
//
// bridgeGoalStateName - a BridgeGoalState as the goal resource spells it
//
extern const char* bridgeGoalStateName(int state);



// -----------------------------------------------------------------------------
//
// bridgeGoalsRender / bridgeGoalRender - a Channel's goals in flight, as Goal bodies
//
// Only while a goal is in progress, as phase 1: an ended goal is gone - its end
// is in TRoE and in the notifications. bridgeGoalRender: NULL when not found.
//
extern KjNode* bridgeGoalsRender(Channel* channelP);
extern KjNode* bridgeGoalRender(Channel* channelP, const char* goalId);



// -----------------------------------------------------------------------------
//
// bridgeGoalAliasOf - the datasetId of a Channel's goal in flight, by the transport's id - NULL if none
//
extern char* bridgeGoalAliasOf(Channel* channelP, const char* goalId);



#endif  // SRC_LIB_BRIDGE_BRIDGEGOAL_H_

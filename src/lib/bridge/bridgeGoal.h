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
// Always sent BEFORE its request's write, and the transport DECIDES before
// anything is stored: the request waits for the goal's first event
// (bridgeGoalAwait) and writes only a goal that was accepted - the value and
// that first event together, in its own write. The goal is HELD meanwhile: its
// later events wait until bridgeGoalRelease(*tokenP) says the write is done.
//
// @param endpoint  where the goal's events are to be notified, or NULL - the
//                  "endpoint" sub-Attribute of the request. The broker then
//                  holds a subscription of its own for that endpoint to THIS
//                  goal's instance, made on its first event and gone after its
//                  last. Only in the broker's memory: never stored, never
//                  listed - it lives exactly as long as the goal. NULL: the
//                  Channel's default, else the Bridge's (bridgeGoalNotifyDefaultSet),
//                  else nowhere.
// @param tokenP    the goal's token
//
// @return the plugin's answer: BRIDGE_OK, or why the goal did not go
//
extern int bridgeGoalSend(Channel* channelP, const char* json, const char* endpoint, uint64_t* tokenP);



// -----------------------------------------------------------------------------
//
// bridgeGoalNotifyDefaultSet - a Bridge's default goal endpoint, for goals that name none
//
// From the configuration ("ngsild": { "notification": { "endpoint": { "uri", "accept" } } }),
// at startup only - the table is read without a lock. accept NULL: application/json.
//
extern void bridgeGoalNotifyDefaultSet(const char* bridgeName, const char* uri, const char* accept);



// -----------------------------------------------------------------------------
//
// bridgeGoalNotifyDefault - a Bridge's default goal endpoint; false if it has none
//
extern bool bridgeGoalNotifyDefault(const char* bridgeName, const char** uriP, const char** acceptP);



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
// BridgeGoalAnswer - a goal's FIRST event, as the request that sent the goal gets it
//
// In the request's arena. subAttrName and json are NULL when the event carried
// no payload - a state change alone.
//
typedef struct BridgeGoalAnswer
{
  int      state;                                     // BridgeGoalState
  bool     final;                                     // the goal ended with it
  char*    goalId;                                    // the transport's id - NULL when it gave none
  char*    goalAlias;                                 // the datasetId of the goal's instance
  char*    subAttrName;
  char*    json;
  int64_t  publishTime;
} BridgeGoalAnswer;



// -----------------------------------------------------------------------------
//
// bridgeGoalAwait - wait for a goal's first event: accepted, or not
//
// ⭐ THE TRANSPORT DECIDES BEFORE ANYTHING IS STORED. A write to an action
// Channel sends its goal, waits here, and then writes only a goal that was
// accepted - so the answer and the data always agree: a rejected goal leaves no
// value, no instance and no history behind it.
//
// dueMs is an absolute CLOCK_MONOTONIC deadline, so that a request sending
// several goals waits for all of them against ONE deadline: its latency is
// the slowest answer, not their sum. An answer that came before the wait began
// is found at once.
//
// Refused (bridgeGoalRefused), or ended with its first event: the goal leaves
// the registry here, and nothing it may still say is written. Accepted: it
// stays, held, for the request's write to make its instance.
//
// @return false: no answer by dueMs. The goal has then been taken out of the
//         registry and CANCELLED - the broker never leaves behind a goal it
//         did not record.
//
extern bool bridgeGoalAwait(uint64_t token, int64_t dueMs, BridgeGoalAnswer* answerP);



// -----------------------------------------------------------------------------
//
// bridgeGoalRefused - does a first event in this state mean the goal was not taken on?
//
extern bool bridgeGoalRefused(int state);



// -----------------------------------------------------------------------------
//
// bridgeGoalAbandon - a goal sent by a request that then writes nothing: cancel it
//
// Nothing about it is written - it is out of the registry before the cancel
// goes. A goal no longer in the registry is left alone.
//
extern void bridgeGoalAbandon(uint64_t token);



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

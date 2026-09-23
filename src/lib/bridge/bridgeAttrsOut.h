#ifndef BRIDGE_BRIDGEATTRSOUT_H_
#define BRIDGE_BRIDGEATTRSOUT_H_

//
// FILE            bridgeAttrsOut.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The write paths' way of saying "these attributes changed" to the bridges.
//
// bridgeAttrOut takes ONE attribute, which is all a PATCH of one attribute
// has. Every other write path - a create, a replace, an entity-level merge, a
// batch of any of them - changes a SET of attributes, and had no way to say so:
// the bridge was reached from two service routines out of sixteen, and a value
// written by any other endpoint simply never left the broker. These are that
// way, and they are deliberately two functions rather than one with a NULL
// argument, because the two callers know two different things:
//
//   ...FromMerge   - "here is what changed", an LdMergeReport. The update
//                    paths compute one already, because subscription matching
//                    and TRoE both need exactly this.
//
//   ...FromEntity  - "here is the whole entity", no report. A create has no
//                    change report because everything in it is new, and a
//                    replace's report describes the diff rather than the body
//                    that was just made true.
//
// Both are a no-op, and a cheap one, when no bridge is loaded or no Channel
// claims the attribute - the two integer loads at the top of bridgeAttrOut.
//
// ⭐ DELETES ARE NOT SENT, and the omission is the decision.
//
// A transport carries values; it has no way to say "this no longer exists".
// DDS's nearest thing is dispose(), which unregisters an instance and is not
// what publishing an empty sample would mean to a subscriber. So a deleted
// attribute is dropped here rather than published as null, empty or last-known
// - each of which would be the broker inventing a statement the application
// never made. When a transport that CAN express a deletion is bridged, it says
// so through the driver, not by this walk guessing.
//
// ⭐ THERE IS STILL NO ECHO LOOP.
//
// An arriving sample is stored by bridgeSampleIn, which writes through the DB
// driver directly and never enters a service routine, so it cannot reach these
// functions - exactly as documented on bridgeAttrOut. That remains a property
// of where the code sits: these are called from the request paths only. A
// DistOp forward arriving from another broker DOES enter a service routine,
// and that is not an echo but a neighbour's update, which is the one case this
// change was missing most.
//

#include "kjson/KjNode.h"                             // KjNode
#include "corNgsild/ldEntityMerge.h"                  // LdMergeReport
#include "db/Tenant.h"                                // Tenant
#include "bridge/bridgeServiceSync.h"                 // BridgeSyncDone



// -----------------------------------------------------------------------------
//
// bridgeAttrsOutFromMerge - publish every attribute an LdMergeReport says changed
//
// @param entityP  the post-write entity, if the caller has it, else NULL. Only
//                 read after a Channel has been found to claim an attribute,
//                 and fetched by bridgeAttrOut if NULL and one has - so a path
//                 that retrieves the entity only for subscriptions does not
//                 have to start retrieving it for bridges.
//
extern void bridgeAttrsOutFromMerge(Tenant* tenantP, const char* entityId, KjNode* entityP, LdMergeReport* reportP, const BridgeSyncDone* syncDoneP);



// -----------------------------------------------------------------------------
//
// bridgeAttrsOutFromEntity - publish every attribute of an entity
//
// For the paths that made a whole entity true at once: create and replace.
// Entity-level members (id, type, @context, scope, the timestamps) are not
// attributes and are skipped by ldIsNotAttributeName, and anything else that
// is not an attribute is filtered by the Channel lookup itself - nothing can
// claim a name no configuration named.
//
extern void bridgeAttrsOutFromEntity(Tenant* tenantP, const char* entityId, KjNode* entityP);

#endif  // BRIDGE_BRIDGEATTRSOUT_H_

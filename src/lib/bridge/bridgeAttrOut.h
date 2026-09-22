#ifndef BRIDGE_BRIDGEATTROUT_H_
#define BRIDGE_BRIDGEATTROUT_H_

//
// FILE            bridgeAttrOut.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include "kjson/KjNode.h"                             // KjNode
#include "db/Tenant.h"                                // Tenant



// -----------------------------------------------------------------------------
//
// bridgeAttrOut - an attribute was written locally; put it on the wire
//
// Called from the write paths after a successful store, with the attribute as
// it was written. If a Channel carries that attribute outbound, its value goes
// to the Channel's endpoint.
//
// A no-op - and a cheap one - when no bridge is loaded or no Channel claims the
// attribute, which is every deployment that does not use bridges.
//
// ⭐ THERE IS NO LOOP, AND NOT BECAUSE OF A GUARD.
//
// An arriving sample is stored by bridgeSampleIn, which writes through the DB
// driver directly and never enters a service routine. So a value that came IN
// from a transport cannot reach this function, which is only ever called from
// the request paths. The echo that would otherwise need a per-request "this
// came from a bridge" flag is absent by construction.
//
// ⚠ That is a property of where the two functions sit, not a law. Routing
// inbound samples through a service routine later would create the loop that
// is currently impossible, and would need the flag this design does without.
//
// @param entityP  the stored entity, if the caller happens to have it, else
//                 NULL. It is only read AFTER a Channel has been found to
//                 claim the attribute, and only fetched if NULL and one has -
//                 so a deployment with no bridges pays two integer loads, and
//                 one with bridges pays nothing extra on attributes no Channel
//                 carries.
//
// What goes on the wire is the attribute's "value" and nothing else. The
// payload belongs to the application; the NGSI-LD wrapper around it is the
// broker's business alone, and means nothing to a publisher on that topic.
//
extern void bridgeAttrOut(Tenant* tenantP, const char* entityId, const char* attrName, KjNode* entityP);

#endif  // BRIDGE_BRIDGEATTROUT_H_

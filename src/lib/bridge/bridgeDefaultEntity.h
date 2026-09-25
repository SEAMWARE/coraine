#ifndef BRIDGE_BRIDGEDEFAULTENTITY_H_
#define BRIDGE_BRIDGEDEFAULTENTITY_H_

//
// FILE            bridgeDefaultEntity.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The catch-all entity: where a sample goes when no Channel claims its endpoint.
//
// A Channel says "this endpoint becomes that attribute", and a transport that
// hands over everything it hears says a great deal no Channel was configured
// for. The ordinary answer is to drop it, and that stays the default. This is
// the other answer: store it anyway, in ONE entity per bridge, under an
// attribute named after the endpoint itself.
//
// ⭐ It is a DIFFERENT THING FROM A CHANNEL, and it is kept apart from one on
// purpose. A Channel is configuration: it names an entity, a type and an
// attribute, it is unique on both of its keys, and it carries values in both
// directions. The catch-all names none of those - the endpoint decides the
// attribute - and it is INBOUND ONLY: a PATCH of one of its attributes has
// nowhere to go, because nothing said which endpoint that attribute belongs
// to. Synthesising a Channel per arriving endpoint would make the two look
// alike in the cache, and then the reverse lookup would start publishing to
// endpoints nobody configured.
//
// What it is FOR is discovery. Point a broker at a live system with no mapping
// at all and every endpoint on it shows up as an attribute - which is how you
// find out what to write in the configuration file. It is also the single best
// thing to show someone.
//
// ⚠ ON by default (since 2026-09-25), for every bridge whose section of the
// configuration file has an "ngsild" part - because Orion-LD does it, and its
// DDS clients must see what they saw (KZ: "not that I like it very much, but
// yes"). The cost stands: a broker that stores every endpoint it hears, unasked,
// is a different product from one that stores what it was configured to store,
// and on a busy domain it is an unbounded entity. So the file can say:
//
//   "defaultEntity": false                                 - off
//   "defaultEntity": true                                  - the derived pair (the default)
//   "defaultEntity": { "id": "urn:...", "type": "Sensor" } - or say it exactly
//
// The derived pair is "urn:ngsi-ld:<bridge>:default" with the bridge's alias
// uppercased as the type, which for the dds bridge is the pair already in use
// elsewhere: urn:ngsi-ld:dds:default, of type DDS.
//

#include <stdbool.h>                                  // bool

#include "db/Tenant.h"                                // Tenant



// -----------------------------------------------------------------------------
//
// bridgeDefaultEntitySet - this bridge stores its unclaimed endpoints
//
// entityId may be NULL, in which case the derived one is used. entityType is
// required, and EXPANDED, like a Channel's - the loader expands it, being the
// one place that already holds the core context.
//
// @return true on success, false if the table is full or the input is unusable.
//
extern bool bridgeDefaultEntitySet(const char* bridgeName, const char* entityId, const char* entityType, Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// bridgeDefaultEntityGet - where this bridge's unclaimed endpoints go, if anywhere
//
// @return false - and touches nothing - when the bridge has no catch-all, which
//         is the default and the common case.
//
extern bool bridgeDefaultEntityGet(const char* bridgeName, const char** entityIdP, const char** entityTypeP, Tenant** tenantPP);



// -----------------------------------------------------------------------------
//
// bridgeDefaultEntityNeedsCreate / bridgeDefaultEntityCreated - the first sample only
//
// A Channel's entity is pre-created at startup, because the configuration says
// it is going to be needed. The catch-all's cannot be: nobody knows an endpoint
// nothing claims exists until one produces a value. So it is created on the
// first such sample, and this pair is what keeps that from becoming a database
// read per sample forever afterwards.
//
// ⚠ It says "this process has already made it", not "it is there". An entity
// deleted while the broker runs makes the next store fail and warn, which is
// exactly what happens to a Channel whose entity is deleted.
//
extern bool bridgeDefaultEntityNeedsCreate(const char* bridgeName);
extern void bridgeDefaultEntityCreated(const char* bridgeName);



// -----------------------------------------------------------------------------
//
// bridgeDefaultEntityCount - how many bridges have one
//
// For the one check on the sample path: zero means the lookup is not worth
// making, and zero is what almost every deployment has.
//
extern int bridgeDefaultEntityCount(void);

#endif  // BRIDGE_BRIDGEDEFAULTENTITY_H_

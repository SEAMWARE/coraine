#ifndef HA_HAEVENT_H_
#define HA_HAEVENT_H_

//
// FILE            HaEvent.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                     // bool

#include "kjson/KjNode.h"                                // KjNode

#include "db/Tenant.h"                                   // Tenant



// -----------------------------------------------------------------------------
//
// HaOp - what happened to it
//
// An upsert covers create AND update on purpose: applying either means the same
// thing to a cache - make my copy match what the other instance has now - and
// the receiving broker cannot know whether it already had the item anyway.
//
typedef enum HaOp
{
  HaOpUpsert,
  HaOpDelete
} HaOp;



// -----------------------------------------------------------------------------
//
// HaKind - what it is
//
typedef enum HaKind
{
  HaSubscription,
  HaRegistration,
  HaContext
} HaKind;



// -----------------------------------------------------------------------------
//
// HaEvent - "another instance did this", independent of how we were told
//
// The point of this type is that haEventApply() never learns which channel the
// event arrived on. Today there is one: mongo change streams, which only the
// mongoc plugin can produce. The haaux server, over a socket, is meant to be the
// other, and for a deployment on a database with no change feed it will be the
// only one. Two channels feeding two apply paths would drift, and silently.
//
// 'apiP' is the NGSI-LD API representation of the thing - never a database
// model. That matters for haaux, whose wire format has to be something another
// implementation could speak too, and it is what makes haaux the fast channel:
// the receiver applies what it was handed instead of going back to the database
// for it. The mongo channel leaves it NULL - it has no wire, it shares the
// database the event came from, so it lets the apply step read the document
// itself, through the very function every other write path uses.
//
typedef struct HaEvent
{
  HaOp         op;
  HaKind       kind;
  Tenant*      tenantP;   // NULL for HaContext - @contexts are not per tenant
  const char*  id;
  KjNode*      apiP;      // NULL: read it from the database (mongo channel)
} HaEvent;



// -----------------------------------------------------------------------------
//
// HaApplyFunc - what a channel does with an event it has built
//
// Handed to the channel when it is started, rather than called by name: a DB
// plugin is a shared object of its own, and a broker symbol it references but
// the broker itself does not is dropped at link time - the plugin then fails to
// load, at run time, on a build that compiled perfectly.
//
typedef bool (*HaApplyFunc)(HaEvent* eventP);

#endif  // HA_HAEVENT_H_

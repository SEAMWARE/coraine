#ifndef BRIDGE_BRIDGESAMPLEIN_H_
#define BRIDGE_BRIDGESAMPLEIN_H_

//
// FILE            bridgeSampleIn.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdint.h>                                   // int64_t

#include "kjson/KjNode.h"                             // KjNode



// -----------------------------------------------------------------------------
//
// BridgeSubAttr - a payload headed for a sub-attribute of its own: the request a reply answers (ABI 7)
//
typedef struct BridgeSubAttr
{
  const char*  name;                                  // the sub-attribute's name, as the plugin spells it
  const char*  json;                                  // the payload, JSON text
  const char*  meta;                                  // the transport's meta about it, or NULL
  int64_t      time;                                  // nanoseconds since the epoch - 0: not said
} BridgeSubAttr;



// -----------------------------------------------------------------------------
//
// bridgeSampleIn - the broker's side of BridgeBroker::sampleIn
//
// A foreign endpoint produced a value. Resolve it to its Channel and do the
// NGSI-LD work: store the attribute, notify whoever subscribed, record the
// temporal event.
//
// ⚠⚠ CALLED FROM A PLUGIN THREAD - a DDS reader, an MQTT network loop. The
// broker did not create this thread and its thread-locals are not initialised.
// Everything that makes the call safe is here, on the broker's side of the
// seam, and not in any plugin.
//
// Signature matches BridgeBroker::sampleIn exactly; the broker installs it
// there.
//
extern int bridgeSampleIn(const char* bridgeName,
                          const char* endpoint,
                          const char* json,
                          int64_t     publishTime);



// -----------------------------------------------------------------------------
//
// bridgeSampleQualifiedIn - the broker's side of BridgeBroker::sampleQualifiedIn
//
// The same work, for the things a request/reply transport delivers that are not
// plain samples: a reply, a piece of feedback, a status, a result.
//
// ⭐ What makes them different is WHERE THEY LAND, and nothing else. A reply
// belongs to the attribute the service is bound to, but it is not that
// attribute's value - the value is what was asked. So it goes in a
// sub-attribute, under whatever name the plugin gives; and when several
// exchanges are in flight on one endpoint, each one's instance is told apart by
// its datasetId, which is what a datasetId is for.
//
// ⛔ The broker does not know, and must not learn, what those names mean. It is
// handed a name and an instance; the convention that chose them belongs to the
// transport and lives in the plugin. See BridgeBroker.h.
//
// @param datasetId    the instance this belongs to, NULL for the default one
// @param subAttrName  the sub-attribute to put the payload in, NULL for the
//                     attribute's own value - which is then exactly
//                     bridgeSampleIn()
//
// ⚠ The target instance must already exist when subAttrName is given: a reply
// is an answer to something this broker sent, and sending it is what created
// the instance. An answer to nothing is dropped and said so, rather than
// conjuring an attribute whose value nobody ever wrote.
//
extern int bridgeSampleQualifiedIn(const char* bridgeName,
                                   const char* endpoint,
                                   const char* datasetId,
                                   const char* subAttrName,
                                   const char* json,
                                   int64_t     publishTime);



// -----------------------------------------------------------------------------
//
// bridgeGoalWrite - one event of a goal, into the goal's own instance
//
// bridgeSampleQualifiedIn() for a goal (bridgeGoal.c): the instance is the
// goal's alias, and the first event creates it, holding requestJson - the goal
// the broker sent - as its value. subAttrName NULL with json NULL writes nothing.
//
extern int bridgeGoalWrite(const char* bridgeName,
                           const char* endpoint,
                           const char* goalAlias,
                           const char* subAttrName,
                           const char* json,
                           int64_t     publishTime,
                           const char* requestJson,
                           const char* meta);



// -----------------------------------------------------------------------------
//
// bridgeGoalInstanceRemove - a finished goal's instance goes, and nothing else
//
// Stored, notified and recorded exactly as a client's own DELETE of that one
// instance (?datasetId=) would be. The attribute and its other instances -
// other goals, and the value that was last asked - stay.
//
extern int bridgeGoalInstanceRemove(const char* bridgeName, const char* endpoint, const char* goalAlias);

// -----------------------------------------------------------------------------
//
// bridgeSampleMetaIn - the broker's side of BridgeBroker::sampleMetaIn (ABI 6)
//
// bridgeSampleIn(), and each member of meta - the transport's curiosities about
// the sample - becomes a Property sub-attribute of the attribute.
//
extern int bridgeSampleMetaIn(const char* bridgeName, const char* endpoint, const char* json, const char* meta, int64_t publishTime);



// -----------------------------------------------------------------------------
//
// bridgeSampleQualifiedMetaIn - bridgeSampleQualifiedIn(), meta on the sub-attribute
//
extern int bridgeSampleQualifiedMetaIn(const char* bridgeName,
                                       const char* endpoint,
                                       const char* datasetId,
                                       const char* subAttrName,
                                       const char* json,
                                       const char* meta,
                                       int64_t     publishTime,
                                       const BridgeSubAttr* requestP);



// -----------------------------------------------------------------------------
//
// bridgeReplySubAttr - a reply, as the sub-attribute it is stored as
//
// Shared by the asynchronous reply path and the synchronous one (ddsSync), so
// that both store a reply identically. See bridgeSampleIn.c.
//
extern KjNode* bridgeReplySubAttr(const char* attrName, const char* subAttrName, const char* json, int64_t publishTime, const char* meta);




// -----------------------------------------------------------------------------
//
// bridgeGoalInstance - a goal's instance, as the request that sent the goal writes it
//
// The request as its value, and the goal's first event - when it carried a
// payload - as its sub-attribute. In the DB model, named goalAlias and unlinked,
// for the request to put in its fragment. NULL when a text is not valid JSON.
//
extern KjNode* bridgeGoalInstance(const char* attrName,
                                  const char* goalAlias,
                                  const char* requestJson,
                                  const char* subAttrName,
                                  const char* json,
                                  int64_t     publishTime,
                                  const char* meta);

#endif  // BRIDGE_BRIDGESAMPLEIN_H_

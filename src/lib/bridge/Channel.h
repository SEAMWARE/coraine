#ifndef BRIDGE_CHANNEL_H_
#define BRIDGE_CHANNEL_H_

//
// FILE            Channel.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // uint64_t

#include "corBridge/BridgeDriver.h"                   // BridgeChannelKind, BridgeDirection

#include "db/Tenant.h"                                // Tenant



// -----------------------------------------------------------------------------
//
// ChannelRetention - what the broker does with a value that crosses a Channel
//
//   Mirror - store it AND put it on the wire. The broker holds the value.
//   Relay  - put it on the wire only. The broker holds nothing.
//
// ⛔ There is no third 'lazy' value. An attribute the broker does not hold and
// fetches on read is a REGISTRATION, not a Channel: that is delegation, which
// is what a registration means. A Channel is for a value that FLOWS.
//
typedef enum ChannelRetention
{
  ChannelRetentionMirror = 0,
  ChannelRetentionRelay  = 1
} ChannelRetention;



// -----------------------------------------------------------------------------
//
// ChannelStatus - whether a Channel is carrying anything
//
//   Available - its Bridge is up; values cross
//   Dormant   - its Bridge is not available, so nothing crosses
//
// ⭐ Dormant is deliberately NOT an error state. The Channel is valid and the
// configuration is correct; the infrastructure has not caught up. Without it
// the only symptom of a bridge plugin that failed to load is no data arriving,
// which is indistinguishable from a quiet sensor.
//
typedef enum ChannelStatus
{
  ChannelStatusAvailable = 0,
  ChannelStatusDormant   = 1
} ChannelStatus;



// -----------------------------------------------------------------------------
//
// Channel - one foreign endpoint bound to one entity attribute
//
// Or, a RECORD Channel (COR_FEATURE_BRIDGE_RECORDS): one foreign endpoint whose samples are records,
// each mapped to entities by recordMapP. It names no entity attribute, so it claims none: the
// one-writer-per-attribute check below is not made for it. Inbound only.
//
// ⭐ The pair (bridgeName, endpoint) is what arrives from the wire, and
// (entityId, attrName) is what it becomes. BOTH are unique across the cache,
// and for different reasons:
//
//   - two Channels on one endpoint: the second could never be reached, because
//     a lookup answers with one Channel and there is no rule for which.
//
//   - two Channels on one attribute: both ARE reached, and they race. The
//     attribute's value then depends on which transport delivered last, which
//     is not debuggable from the outside. One writer per attribute.
//
typedef struct Channel
{
  char*              id;                              // stored-object id
  char*              bridgeName;                      // the Bridge this Channel is carried by ("dds")
  char*              endpoint;                        // transport-native name, exactly as the wire spells it ("rt/pose")
  BridgeChannelKind  kind;                            // topic | service | action
  BridgeDirection    direction;                       // in | out | both
  ChannelRetention   retention;                       // mirror | relay
  Tenant*            tenantP;                         // which tenant the entity lives in
  char*              entityId;
  char*              entityType;                      // EXPANDED
  char*              attrName;                        // EXPANDED
  ChannelStatus      status;
  char*              statusReason;                    // why, when dormant. NULL otherwise
  char*              notifyUri;                       // action: where a goal naming no endpoint is notified. NULL: the Bridge's default
  char*              notifyAccept;                    // its accept - application/json unless the file said otherwise
  char*              info;                            // channelInfo as JSON text (an array of {key, value}), NULL if none - for the plugin (channelAddInfo, ABI 9)
  struct RecordMap*  recordMapP;                      // a RECORD Channel: how a record becomes entities (RecordMap.h) - entityId, entityType and attrName are then NULL. NULL: a plain Channel
  char**             retiredReasonV;                  // statusReasons replaced while the Channel was read (channelStatusSet) - freed with the Channel
  int                retiredReasons;

  //
  // What the transport said, and what crossed - rendered by GET /channels. A test waits on THESE,
  // never on the log: a trace depends on the trace levels, and on how the plugin was built.
  // Updated with __atomic builtins (plugin threads and workers alike), read the same way.
  //
  bool               endpointDiscovered;              // the transport reported finding the endpoint (endpointDiscoveredIn, ABI 8)
  uint64_t           samplesIn;                       // samples written to the entity
  uint64_t           samplesOut;                      // values published
  uint64_t           goalsSent;                       // goals the transport took (actionGoalSend returned OK)
  uint64_t           goalCancelsSent;                 // cancels sent: asked for (DELETE), or of a goal whose request wrote nothing
  uint64_t           requestsNotWaited;               // requests sent without waiting for the reply - every wait slot taken
  int                requestsWaiting;                 // requests waiting for their reply right now

  struct Channel*    next;
} Channel;

#endif  // BRIDGE_CHANNEL_H_

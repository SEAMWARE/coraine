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
  struct Channel*    next;
} Channel;

#endif  // BRIDGE_CHANNEL_H_

#ifndef BRIDGE_DATACHANNEL_H_
#define BRIDGE_DATACHANNEL_H_

//
// FILE            DataChannel.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// DataChannel - one foreign endpoint whose samples are RECORDS (a CKAN row, a CSV line: a JSON object, one
// member per column), each made into entities by the DataChannel's mapping (RecordMap.h).
//
// Not a Channel. A Channel binds an endpoint to ONE attribute of ONE entity and a sample is that
// attribute's value; a record carries many values, about many entities, and names its entities INSIDE
// it. So a DataChannel names no entity and no attribute, claims none (no one-writer rule: the last
// writer wins, TRoE keeps what was there), has no kind, no direction (inbound only), no goals and no
// pre-population. It has its own cache, its own listing (GET /ngsi-ld/v1/dataChannels) and its own
// counters.
//
// What it shares with a Channel is the seam: the bridge carries (endpoint, json, time) and knows nothing
// about entities; it is handed the endpoint as a topic, inbound, with the entry's channelInfo
// (channelAddInfo), and reports on it with channelStatusIn - one (bridge, endpoint) is a Channel or a
// DataChannel, never both.
//
// Compiled in with COR_FEATURE_DATA_CHANNELS.
//
#include <stdint.h>                                   // uint64_t

#include "db/Tenant.h"                                // Tenant
#include "bridge/Channel.h"                           // ChannelStatus
#include "bridge/RecordMap.h"                         // RecordMap



// -----------------------------------------------------------------------------
//
// DataChannel -
//
typedef struct DataChannel
{
  char*                bridgeName;                    // the Bridge's alias
  char*                endpoint;                      // the endpoint, the bridge's name for it (a CKAN resource id)
  Tenant*              tenantP;                       // whose entities it writes
  RecordMap*           mapP;                          // how a record becomes entities
  char*                info;                          // channelInfo as JSON text (an array of {key, value}), NULL if none - for the plugin

  ChannelStatus        status;                        // available, or dormant - channelStatusIn on a plugin thread: atomic
  char*                statusReason;                  // why dormant, or what the plugin said; NULL: none - atomic
  char**               retiredReasonV;                // statusReasons replaced while the DataChannel was read - freed with it
  int                  retiredReasons;

  uint64_t             recordsIn;                     // records that wrote at least one entity
  uint64_t             recordsRefused;                // records that wrote none: not an object, no entity id, every entity refused
  uint64_t             entitiesWritten;               // entities created or updated

  struct DataChannel*  next;
} DataChannel;

#endif  // BRIDGE_DATACHANNEL_H_

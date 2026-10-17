#ifndef BRIDGE_ASSETCHANNEL_H_
#define BRIDGE_ASSETCHANNEL_H_

//
// FILE            AssetChannel.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// AssetChannel - one foreign endpoint whose data is a MODEL: an OPC UA server's objects, an AAS shell and its
// submodels. One asset is a tree of nodes, typed and with meaning attached, with references across it, and
// it becomes a graph of entities, with Relationships where the model has references (doc/asset-channels.md).
//
// Not a Channel (one endpoint, one attribute) and not a DataChannel (a record, a few entities): its own
// cache, its own listing (GET /ngsi-ld/v1/assetChannels) and its own counters. What it shares with both is
// the seam - the bridge carries (endpoint, json, time) and knows nothing about entities. On an
// AssetChannel's endpoint (its channelTarget: the model's root), the json is a neutral node tree:
//
//   { "model":  <node> }                                       the model, read whole: written, and kept
//   { "values": [ { "id": "<node id>", "value": <json>, "sourceTime": "<ISO 8601>" }, ... ] }
//                                                              values that changed: the kept model updated,
//                                                              the entities they are in written again
//
//   <node>: { "node": "<kind>", "id": "<the source's id>", "name": "<its name>", "typeName": "<its type's name>",
//             "typeDefinition": "<id>", "semanticIds": [ "<id>", ... ],
//             "value": <json>, "unit": "<UN/CEFACT code>", "sourceTime": "<ISO 8601>",
//             "children": [ <node>, ... ], "references": [ { "type": "<name>", "target": "<node id>" }, ... ] }
//
// The same tree from every model bridge - OPC UA, AAS - so that every model maps the same way, here.
//
// Like a DataChannel: inbound only, no owner (the last write wins), each write one batch upsert with
// options=update, local - created when not there, the mapped attributes replaced, the others left alone.
//
// Compiled in with COR_FEATURE_ASSET_CHANNELS.
//
#include <pthread.h>                                  // pthread_mutex_t
#include <stdint.h>                                   // uint64_t

#include "db/Tenant.h"                                // Tenant
#include "bridge/Channel.h"                           // ChannelStatus
#include "bridge/AssetMap.h"                          // AssetMap



// -----------------------------------------------------------------------------
//
// AssetChannel -
//
typedef struct AssetChannel
{
  char*                 bridgeName;                   // the Bridge's alias
  char*                 endpoint;                     // the model's root, the bridge's name for it (an OPC UA NodeId, an AAS id)
  Tenant*               tenantP;                      // whose entities it writes
  AssetMap*             mapP;                         // which nodes become entities, and how
  char*                 info;                         // channelInfo as JSON text (an array of {key, value}), NULL if none - for the plugin

  pthread_mutex_t       modelMutex;                   // model: replaced by a "model", updated by "values"
  char*                 model;                        // the last model, as JSON text (malloc); NULL before the first

  ChannelStatus         status;                       // available, or dormant - channelStatusIn on a plugin thread: atomic
  char*                 statusReason;                 // why dormant, or what the plugin said; NULL: none - atomic
  char**                retiredReasonV;               // statusReasons replaced while the AssetChannel was read - freed with it
  int                   retiredReasons;

  uint64_t              modelsIn;                     // models written
  uint64_t              valuesIn;                     // node values written
  uint64_t              valuesRefused;                // node values of no node of the model (or before the first model)
  uint64_t              entitiesWritten;              // entities created or updated
  uint64_t              samplesRefused;               // samples that are neither a model nor values, or a model that makes no entity
#if COR_FEATURE_CHANNEL_CHECKS
  uint64_t              payloadsRefused;              // client writes the bridge's payloadCheck refused (ABI 13)
#endif

  struct AssetChannel*  next;
} AssetChannel;

#endif  // BRIDGE_ASSETCHANNEL_H_

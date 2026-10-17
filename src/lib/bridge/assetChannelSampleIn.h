#ifndef BRIDGE_ASSETCHANNEL_SAMPLE_IN_H_
#define BRIDGE_ASSETCHANNEL_SAMPLE_IN_H_

//
// FILE            assetChannelSampleIn.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdint.h>                                   // int64_t

#include "bridge/AssetChannel.h"                      // AssetChannel



// -----------------------------------------------------------------------------
//
// assetChannelSampleIn - a sample on an AssetChannel: a model, or values that changed (AssetChannel.h)
//
// { "model": <node> } - the model is kept, and every entity its mapping (AssetMap.h) makes of it is written.
// { "values": [ ... ] } - each node's value is set in the kept model, and the entities those nodes are in
// written again. A value of no node of the model (or before the first model) is counted, and dropped.
//
// Each write is ONE batch upsert, options=update (bridgeEntitiesUpsert): an entity that is not there is
// created, its mapped attributes replaced, the others left alone. Entities and attributes the model no longer
// has are left alone too.
//
// On the plugin's thread, from bridgeSampleIn. publishTime is not used: a node says its own sourceTime.
//
// @return BRIDGE_OK (written), BRIDGE_BAD_INPUT (neither a model nor values, a model that makes no entity,
//         values of no node), BRIDGE_ERR (nothing written - the reasons logged)
//
extern int assetChannelSampleIn(AssetChannel* acP, const char* json, int64_t publishTime);

#endif  // BRIDGE_ASSETCHANNEL_SAMPLE_IN_H_

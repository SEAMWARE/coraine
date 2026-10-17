#ifndef BRIDGE_ASSETCHANNELRENDER_H_
#define BRIDGE_ASSETCHANNELRENDER_H_

//
// FILE            assetChannelRender.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                          // CorNode

#include "bridge/AssetChannel.h"                      // AssetChannel



// -----------------------------------------------------------------------------
//
// assetChannelIdOf - urn:ngsi-ld:AssetChannel:<bridge>:<endpoint>, in the request's allocator
//
extern const char* assetChannelIdOf(AssetChannel* acP);



// -----------------------------------------------------------------------------
//
// assetChannelRender - a AssetChannel as GET /ngsi-ld/v1/assetChannels shows it
//
extern CorNode* assetChannelRender(AssetChannel* acP);

#endif  // BRIDGE_ASSETCHANNELRENDER_H_

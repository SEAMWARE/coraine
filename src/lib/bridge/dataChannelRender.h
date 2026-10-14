#ifndef BRIDGE_DATACHANNELRENDER_H_
#define BRIDGE_DATACHANNELRENDER_H_

//
// FILE            dataChannelRender.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                          // CorNode

#include "bridge/DataChannel.h"                       // DataChannel



// -----------------------------------------------------------------------------
//
// dataChannelIdOf - urn:ngsi-ld:DataChannel:<bridge>:<endpoint>, in the request's allocator
//
extern const char* dataChannelIdOf(DataChannel* dcP);



// -----------------------------------------------------------------------------
//
// dataChannelRender - a DataChannel as GET /ngsi-ld/v1/dataChannels shows it
//
extern CorNode* dataChannelRender(DataChannel* dcP);

#endif  // BRIDGE_DATACHANNELRENDER_H_

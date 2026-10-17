#ifndef BRIDGE_ASSETCHANNELCACHE_H_
#define BRIDGE_ASSETCHANNELCACHE_H_

//
// FILE            assetChannelCache.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The AssetChannels (AssetChannel.h) - every one from the configuration file, added before the bridges
// start and kept until the broker stops: the list does not change while a bridge thread can read it, so a
// lookup takes no lock. What changes - the status, its reason, the counters - is read and written
// atomically.
//
#include "db/Tenant.h"                                // Tenant
#include "bridge/Channel.h"                           // ChannelStatus
#include "bridge/AssetChannel.h"                      // AssetChannel
#include "bridge/AssetMap.h"                          // AssetMap



// -----------------------------------------------------------------------------
//
// assetChannelCreate - add a AssetChannel; it takes mapP and info over, on CHANNEL_OK only
//
// CHANNEL_DUP_ENDPOINT: (bridgeName, endpoint) is a Channel's or another AssetChannel's.
// CHANNEL_BAD_INPUT:    a name missing, or one with the Channel key separator in it.
// CHANNEL_ERR:          out of memory - or no Bridge of that name is loaded: the AssetChannel is then added,
//                       dormant, as a Channel is.
//
extern int assetChannelCreate(const char* bridgeName, const char* endpoint, Tenant* tenantP, AssetMap* mapP, char* info);



// -----------------------------------------------------------------------------
//
// assetChannelLookup - the AssetChannel of (bridgeName, endpoint), or NULL
//
extern AssetChannel* assetChannelLookup(const char* bridgeName, const char* endpoint);



// -----------------------------------------------------------------------------
//
// assetChannelLookupById - urn:ngsi-ld:AssetChannel:<bridge>:<endpoint> of tenantP, or NULL
//
extern AssetChannel* assetChannelLookupById(const char* id, Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// assetChannelFirst - the list, for a walk (->next)
//
extern AssetChannel* assetChannelFirst(void);



// -----------------------------------------------------------------------------
//
// assetChannelStatusSet - a AssetChannel's status and statusReason, as its bridge reports them
//
// Any thread. reason is copied; NULL for none. CHANNEL_ERR: no such AssetChannel.
//
extern int assetChannelStatusSet(const char* bridgeName, const char* endpoint, ChannelStatus status, const char* reason);



// -----------------------------------------------------------------------------
//
// assetChannelsRelease - every AssetChannel freed, when the broker stops (after the bridges)
//
extern void assetChannelsRelease(void);

#endif  // BRIDGE_ASSETCHANNELCACHE_H_

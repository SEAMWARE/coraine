#ifndef BRIDGE_DATACHANNELCACHE_H_
#define BRIDGE_DATACHANNELCACHE_H_

//
// FILE            dataChannelCache.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The DataChannels (DataChannel.h) - every one from the configuration file, added before the bridges
// start and kept until the broker stops: the list does not change while a bridge thread can read it, so a
// lookup takes no lock. What changes - the status, its reason, the counters - is read and written
// atomically.
//
#include "db/Tenant.h"                                // Tenant
#include "bridge/Channel.h"                           // ChannelStatus
#include "bridge/DataChannel.h"                       // DataChannel
#include "bridge/RecordMap.h"                         // RecordMap



// -----------------------------------------------------------------------------
//
// dataChannelCreate - add a DataChannel; it takes mapP and info over, on CHANNEL_OK only
//
// CHANNEL_DUP_ENDPOINT: (bridgeName, endpoint) is a Channel's or another DataChannel's.
// CHANNEL_BAD_INPUT:    a name missing, or one with the Channel key separator in it.
// CHANNEL_ERR:          out of memory - or no Bridge of that name is loaded: the DataChannel is then added,
//                       dormant, as a Channel is.
//
extern int dataChannelCreate(const char* bridgeName, const char* endpoint, Tenant* tenantP, RecordMap* mapP, char* info);



// -----------------------------------------------------------------------------
//
// dataChannelLookup - the DataChannel of (bridgeName, endpoint), or NULL
//
extern DataChannel* dataChannelLookup(const char* bridgeName, const char* endpoint);



// -----------------------------------------------------------------------------
//
// dataChannelLookupById - urn:ngsi-ld:DataChannel:<bridge>:<endpoint> of tenantP, or NULL
//
extern DataChannel* dataChannelLookupById(const char* id, Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// dataChannelFirst - the list, for a walk (->next)
//
extern DataChannel* dataChannelFirst(void);



#if COR_FEATURE_CHANNEL_WRITE_BACK
// -----------------------------------------------------------------------------
//
// dataChannelDirectionSet - at start, before any request: Both counts in dataChannelOutCount
//
extern void dataChannelDirectionSet(DataChannel* dcP, BridgeDirection direction);



// -----------------------------------------------------------------------------
//
// dataChannelOutCount - how many DataChannels write back (channelDirection both)
//
extern int dataChannelOutCount(void);
#endif



// -----------------------------------------------------------------------------
//
// dataChannelStatusSet - a DataChannel's status and statusReason, as its bridge reports them
//
// Any thread. reason is copied; NULL for none. CHANNEL_ERR: no such DataChannel.
//
extern int dataChannelStatusSet(const char* bridgeName, const char* endpoint, ChannelStatus status, const char* reason);



// -----------------------------------------------------------------------------
//
// dataChannelsRelease - every DataChannel freed, when the broker stops (after the bridges)
//
extern void dataChannelsRelease(void);

#endif  // BRIDGE_DATACHANNELCACHE_H_

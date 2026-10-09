#ifndef BRIDGE_CHANNELCACHE_H_
#define BRIDGE_CHANNELCACHE_H_

//
// FILE            channelCache.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include "bridge/Channel.h"                           // Channel, ChannelRetention, ChannelStatus



// -----------------------------------------------------------------------------
//
// Creation outcomes
//
#define CHANNEL_OK                 0
#define CHANNEL_ERR               -1   // out of memory, or no Bridge named
#define CHANNEL_DUP_ENDPOINT      -2   // another Channel already carries this (bridge, endpoint)
#define CHANNEL_DUP_TARGET        -3   // another Channel already writes this entity attribute
#define CHANNEL_BAD_INPUT         -4   // a mandatory member is missing



// -----------------------------------------------------------------------------
//
// channelCacheInit - prepare the cache
//
extern int channelCacheInit(void);



// -----------------------------------------------------------------------------
//
// channelCreate - add a Channel to the cache
//
// Refuses on either kind of collision - see Channel.h for why both are
// collisions. On CHANNEL_DUP_ENDPOINT or CHANNEL_DUP_TARGET, *clashPP is set to
// the Channel already holding the claim, so the caller can name it in the
// error; a caller that does not care may pass NULL.
//
// ⭐ The Bridge is resolved here: if no loaded bridge plugin answers to
// bridgeName, the Channel is still created, with status Dormant and a reason.
// A configuration that names a transport this build was not started with is
// valid configuration that cannot currently be served - not an error.
//
extern int channelCreate(const char*        id,
                         const char*        bridgeName,
                         const char*        endpoint,
                         BridgeChannelKind  kind,
                         BridgeDirection    direction,
                         ChannelRetention   retention,
                         Tenant*            tenantP,
                         const char*        entityId,
                         const char*        entityType,
                         const char*        attrName,
                         Channel**          clashPP);



#if COR_FEATURE_BRIDGE_RECORDS
// -----------------------------------------------------------------------------
//
// channelRecordsCreate - add a RECORD Channel: an endpoint whose samples are records, made into
//                        entities by recordMapP (RecordMap.h)
//
// A topic, inbound only, mirror. It claims no attribute, so the only collision is on the endpoint
// (CHANNEL_DUP_ENDPOINT). The Channel takes recordMapP over - on CHANNEL_OK only.
//
extern int channelRecordsCreate(const char* bridgeName, const char* endpoint, Tenant* tenantP, struct RecordMap* recordMapP, Channel** clashPP);
#endif



#if COR_FEATURE_CHANNEL_STATUS_IN
// -----------------------------------------------------------------------------
//
// channelStatusSet - a Channel's status and statusReason, as its bridge reports them (channelStatusIn)
//
// Any thread. reason is copied; NULL for none. CHANNEL_ERR: no such Channel.
//
extern int channelStatusSet(const char* bridgeName, const char* endpoint, ChannelStatus status, const char* reason);



// -----------------------------------------------------------------------------
//
// bridgeChannelStatusIn - the broker's side of BridgeBroker.channelStatusIn (ABI 12): channelStatusSet
//
extern int bridgeChannelStatusIn(const char* bridgeName, const char* endpoint, int status, const char* reason);
#endif



// -----------------------------------------------------------------------------
//
// channelLookup - find the Channel carrying (bridgeName, endpoint)
//
// ⚠ THE HOT PATH: called once per arriving sample, from a plugin thread.
//
extern Channel* channelLookup(const char* bridgeName, const char* endpoint);



// -----------------------------------------------------------------------------
//
// channelLookupByTarget - find the Channel that writes an entity attribute
//
// The reverse index, and the reason one exists: it is what makes "one writer
// per attribute" checkable. Cold - creation time only.
//
extern Channel* channelLookupByTarget(Tenant* tenantP, const char* entityId, const char* attrName);



// -----------------------------------------------------------------------------
//
// channelDelete - remove a Channel from the cache
//
extern int channelDelete(const char* bridgeName, const char* endpoint);



// -----------------------------------------------------------------------------
//
// channelCacheFirst / channelCount - iterate, and how many there are
//
extern Channel* channelCacheFirst(void);
extern int      channelCount(void);



// -----------------------------------------------------------------------------
//
// channelOutCount - how many Channels SEND: every one that is not inbound only
//
// What decides whether a write has to look for something to send before it
// stores (bridgeRequestsBeforeWrite - a service request, a goal, a topic's
// sample: DDS first). Not channelCount(): a bridge that only listens - an MQTT
// subscription, say - would make every write, every batch fragment, look up a
// Channel per attribute for sends that cannot happen.
//
extern int      channelOutCount(void);

#endif  // BRIDGE_CHANNELCACHE_H_

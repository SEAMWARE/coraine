#ifndef SRC_LIB_BRIDGE_BRIDGERENDER_H_
#define SRC_LIB_BRIDGE_BRIDGERENDER_H_

//
// FILE            bridgeRender.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                  // bool

#include "kjson/KjNode.h"                             // KjNode
#include "corJsonld/CorLdContext.h"                   // CorLdContext

#include "db/Tenant.h"                                // Tenant
#include "bridge/Channel.h"                           // Channel



// -----------------------------------------------------------------------------
//
// channelIdOf - a Channel's id: its stored one, else derived from what it carries
//
// A Channel from the configuration file has no stored id, so it is named by
// what makes it unique - its Bridge and its endpoint on that Bridge:
// urn:ngsi-ld:Channel:<bridge>:<endpoint>. The same Channel gets the same id
// on every start, which is what lets a client keep it.
//
extern const char* channelIdOf(Channel* channelP);



// -----------------------------------------------------------------------------
//
// channelOfTenant - a tenant's Channel by its id (channelIdOf) - NULL if none
//
extern Channel* channelOfTenant(Tenant* tenantP, const char* channelId);



// -----------------------------------------------------------------------------
//
// bridgeIdOf - urn:ngsi-ld:ContextBridge:<name>
//
extern const char* bridgeIdOf(const char* bridgeName);



// -----------------------------------------------------------------------------
//
// channelRender - a Channel as its JSON-LD body
//
// Entity type and Attribute name are compacted with the request's @context.
//
extern KjNode* channelRender(Channel* channelP, CorLdContext* contextP);



// -----------------------------------------------------------------------------
//
// bridgeRender - a ContextBridge as its JSON-LD body
//
// loaded: its plugin is one the broker was started with. A Bridge that only
// Channels name is unavailable - the configuration is valid, the transport is
// not here (bridge-channels.md 3.3a).
//
extern KjNode* bridgeRender(const char* bridgeName, bool loaded);

#endif  // SRC_LIB_BRIDGE_BRIDGERENDER_H_

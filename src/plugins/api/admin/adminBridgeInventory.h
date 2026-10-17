//
// FILE            adminBridgeInventory.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#ifndef ADMIN_BRIDGE_INVENTORY_H
#define ADMIN_BRIDGE_INVENTORY_H

#include <stdbool.h>                              // bool



// -----------------------------------------------------------------------------
//
// ADMIN_INVENTORY_PARAMS - ?from and ?depth of GET /admin/bridges/{bridge}/inventory (one bit, both)
//
#define ADMIN_INVENTORY_PARAMS    (1ULL << 61)



// -----------------------------------------------------------------------------
//
// adminGetBridgeInventory - GET /admin/bridges/{bridge}/inventory[?from=<where>][&depth=<n>]
//
// What a bridge's transport can carry, as the bridge reports it (BridgeDriver.inventory, bridge ABI 14) - for a
// mapping tool (c3's Configure) to choose Channels and AssetChannels from. The JSON is the bridge's, its shape
// the bridge's documentation's (the OPC UA bridge: the server's node tree). ?from: where to start, in the
// transport's terms (OPC UA: a NodeId; none: the Objects folder); ?depth: how many levels (default 1).
//
// 404: no such bridge loaded, or no such 'from'; 501: the bridge has no inventory; 503: its transport cannot
// answer (not connected, timed out) - the bridge's reason in detail.
//
extern bool adminGetBridgeInventory(void);

#endif  // ADMIN_BRIDGE_INVENTORY_H

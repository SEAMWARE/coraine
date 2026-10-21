#ifndef SRC_LIB_BRIDGE_BRIDGEHTTPLISTEN_H_
#define SRC_LIB_BRIDGE_BRIDGEHTTPLISTEN_H_

//
// FILE            bridgeHttpListen.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// A port the broker serves for a bridge plugin (BridgeBroker.httpListen, ABI 16): where a device is the
// client - an IoT Agent's southbound port, a webhook a cloud calls back. Each port is a corHttp server
// of its own, one event loop on a thread of its own, its requests handed to the plugin's handler: no
// NGSI-LD route, no auth plugin, nothing of the broker's API. Either HTTP server build has it.
//
#include "corBridge/BridgeBroker.h"                   // BridgeHttpHandler



// -----------------------------------------------------------------------------
//
// bridgeHttpListen - BridgeBroker.httpListen
//
extern int bridgeHttpListen(const char* bridgeName, unsigned short port, BridgeHttpHandler handler);



// -----------------------------------------------------------------------------
//
// bridgeHttpListenStop - stop serving the ports of a bridge, before its close(): when this returns no
// handler of the bridge runs, nor will
//
extern void bridgeHttpListenStop(const char* bridgeName);

#endif  // SRC_LIB_BRIDGE_BRIDGEHTTPLISTEN_H_

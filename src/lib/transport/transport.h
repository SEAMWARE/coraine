#ifndef CORAINE_TRANSPORT_H_
#define CORAINE_TRANSPORT_H_

//
// FILE            transport.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The broker's side of the transport plugins (plugin/TransportDriver.h, doc/websocket.md): loading them,
// the HTTP upgrade that hands them a connection, the messages - requests in the envelope of the MQTT
// notification binding - run through the same service routines as HTTP, and the notifications to
// their connections.
//
#include <stdbool.h>                                 // bool



// -----------------------------------------------------------------------------
//
// transportLoad - the plugins of --transports (comma-separated, from <plugins>/transport/); 0 on success
//
extern int transportLoad(const char* commaList, char* errorBuf, int errorBufSize);



// -----------------------------------------------------------------------------
//
// transportInit - every loaded plugin initialised, and the HTTP upgrade hook set; false: a plugin refused
//
extern bool transportInit(void);



// -----------------------------------------------------------------------------
//
// transportNotifyHas / transportNotifySend - a notification endpoint that names a transport connection
// (urn:ngsi-ld:WebSocket:<bootId>-<n>): does it exist, and the envelope to it. corNgsild's transport hook asks
// these before the bridges (bridgeNotify.c).
//
extern bool transportNotifyHas(const char* uri);
extern bool transportNotifySend(const char* uri, const char* payload);



// -----------------------------------------------------------------------------
//
// transportUri / transportEndpointOwn - a URI that names a transport connection (of this run or not), and
// whether it names the connection whose own request runs on this thread - the only one a subscription may
// name as its endpoint (bridgeNotify.c, at create and update)
//
extern bool transportUri(const char* uri);
extern bool transportEndpointOwn(const char* uri);



// -----------------------------------------------------------------------------
//
// transportLeftoversDelete - every subscription and context source subscription that notifies a connection
// of another run (a broker killed or crashed), deleted in every tenant; after corRestInit, and not with
// --high-availability (another instance's connections are of another run too)
//
extern void transportLeftoversDelete(void);



// -----------------------------------------------------------------------------
//
// transportStop - every plugin stopped (its connections closed)
//
extern void transportStop(void);

#endif  // CORAINE_TRANSPORT_H_

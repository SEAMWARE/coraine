#ifndef CORAINE_TRANSPORTDRIVER_H_
#define CORAINE_TRANSPORTDRIVER_H_

//
// FILE            TransportDriver.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// A TRANSPORT plugin carries the broker's API over a protocol that is not HTTP - requests in, responses
// and notifications out (doc/websocket.md § 1). Not a bridge: a bridge carries attribute values and
// never answers a request.
//
// The plugin knows its protocol - a WebSocket's handshake and frames - and its connections; the broker
// knows the messages (the envelope of the MQTT notification binding: { "metadata", "body" }), NGSI-LD,
// and which connection a subscription notifies. Between them: text messages and connection ids.
//
// The plugin exports one function, transportRegister(TransportDriver*), and fills in the struct. The
// struct is the broker's, allocated at the size of ITS header: abiVersion says how much room there is
// before the plugin writes - append-only, as for bridges (corBridge's BridgeDriver.h).
//
#include <stdbool.h>                                 // bool



#define TRANSPORT_ABI_VERSION  1



// -----------------------------------------------------------------------------
//
// TransportHost - what the broker offers the plugin (the plugin keeps the pointer it gets in init)
//
typedef struct TransportHost
{
  int    abiVersion;

  //
  // opened - a connection is up: the broker greets it (its id, in the first message)
  // message - a text message arrived on it: the broker answers through the driver's send, on this thread
  //           (a request is run to its end here - the plugin calls this on a thread of its own, never on
  //           the HTTP server's)
  // closed - it is gone: the broker deletes the subscriptions that notify it
  //
  void (*opened)(int connId);
  void (*message)(int connId, const char* text, int len);
  void (*closed)(int connId);
} TransportHost;



// -----------------------------------------------------------------------------
//
// TransportDriver - what the plugin offers the broker
//
typedef struct TransportDriver
{
  int          abiVersion;                           // set by the BROKER before transportRegister; then the plugin's
  const char*  name;                                 // "ws"
  const char*  upgrade;                              // the HTTP Upgrade it takes: "websocket"

  bool  (*init)(TransportHost* hostP, char* errorBuf, int errorBufSize);

  //
  // handshake - an upgrade request: its headers (header(key), case-insensitive) in, the 101's headers out
  // (respHeader). false: refused - *statusP and *detailP say why (400, "no Sec-WebSocket-Key").
  //
  bool  (*handshake)(const char* (*header)(const char* key), void (*respHeader)(const char* key, const char* value), int* statusP, const char** detailP);

  //
  // take - the socket, once the 101 is written, and what the client sent behind the request; the
  // plugin's until it calls closeFn(closeArg) - never close() itself. 'connId' is the broker's name for
  // the connection, registered before this call (so the plugin's thread may call opened at once).
  // Called on the HTTP server's thread: the plugin hands the connection to a thread of its own and
  // returns. false: refused (the plugin closed it).
  //
  bool  (*take)(int connId, int fd, const char* extra, int extraLen, void (*closeFn)(void*), void* closeArg);

  bool  (*send)(int connId, const char* text, int len);   // a text message to a connection; any thread
  void  (*stop)(void);
} TransportDriver;

typedef void (*TransportRegisterFunc)(TransportDriver* driverP);

#endif  // CORAINE_TRANSPORTDRIVER_H_

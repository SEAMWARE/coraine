//
// FILE            bridgeNotify.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                  // bool
#include <string.h>                                   // strstr, strchr, strncmp

#include "corBridge/BridgeDriver.h"                   // BridgeDriver, bridges, bridgeCount
#include "corBridge/BridgeBroker.h"                   // BRIDGE_OK
#include "corNgsild/ldNotifyTransport.h"              // ldNotifyTransportSet
#include "transport/transport.h"                       // transportNotifyHas, transportNotifySend

#include "bridge/bridgeNotify.h"                      // Own interface



// -----------------------------------------------------------------------------
//
// tlsInsecure - --insecureNotif, handed to every notify()
//
static bool tlsInsecure = false;



// -----------------------------------------------------------------------------
//
// schemeIn - is the URI's scheme (what precedes "://") one of a comma-separated list?
//
static bool schemeIn(const char* uri, const char* schemes)
{
  const char* sep = strstr(uri, "://");

  if (sep == NULL)
    return false;

  int len = sep - uri;

  const char* s = schemes;

  while ((s != NULL) && (*s != 0))
  {
    const char* comma = strchr(s, ',');
    int         sLen  = (comma != NULL) ? (int) (comma - s) : (int) strlen(s);

    if ((sLen == len) && (strncmp(s, uri, len) == 0))
      return true;

    s = (comma != NULL) ? &comma[1] : NULL;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// notifyBridge - the loaded bridge that delivers notifications to this URI's scheme, or NULL
//
static BridgeDriver* notifyBridge(const char* uri)
{
  for (int i = 0; i < bridgeCount; i++)
  {
    BridgeDriver* driverP = &bridges[i];

    //
    // ABI 10 or later: an older plugin's struct ends before these slots - the host zeroed them, but
    // the plugin's own ABI is what says whether it meant anything by them
    //
    if ((driverP->abiVersion < 10) || (driverP->notifySchemes == NULL) || (driverP->notify == NULL))
      continue;

    if (schemeIn(uri, driverP->notifySchemes) == true)
      return driverP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// transportHas - corNgsild's question at Subscription create/update
//
static bool transportHas(const char* uri)
{
  return transportNotifyHas(uri) || (notifyBridge(uri) != NULL);   // a transport's connection, or a bridge's scheme
}



// -----------------------------------------------------------------------------
//
// transportSend - corNgsild's delivery of one notification
//
// A Subscription stored before a restart without its bridge still names the scheme: no bridge, a
// failed notification - counted on the Subscription like any other.
//
static bool transportSend(const char* uri, const char* payload, const char* notifierInfoJson)
{
  if (transportNotifyHas(uri) == true)
    return transportNotifySend(uri, payload);       // a WebSocket connection: the envelope, as is

  BridgeDriver* driverP = notifyBridge(uri);

  if (driverP == NULL)
    return false;

  return driverP->notify(uri, payload, notifierInfoJson, tlsInsecure) == BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// bridgeNotifyInit -
//
void bridgeNotifyInit(bool _tlsInsecure)
{
  tlsInsecure = _tlsInsecure;
  ldNotifyTransportSet(transportHas, transportSend);
}

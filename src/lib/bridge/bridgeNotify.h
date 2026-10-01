#ifndef BRIDGE_BRIDGENOTIFY_H_
#define BRIDGE_BRIDGENOTIFY_H_

//
// FILE            bridgeNotify.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                  // bool



// -----------------------------------------------------------------------------
//
// bridgeNotifyInit - deliver non-HTTP notifications through the bridge plugins
//
// Installs corNgsild's notification-transport hook: a Subscription whose endpoint scheme is claimed
// by a loaded bridge (BridgeDriver.notifySchemes, ABI 10 - "mqtt,mqtts" for mqtt.so) is notified
// through that bridge's notify(). A scheme no loaded bridge claims is refused when the Subscription
// is created - so mqtt:// notifications need the broker started with --bridges mqtt.
//
// tlsInsecure - --insecureNotif: an mqtts:// endpoint's self-signed certificate is accepted
//
extern void bridgeNotifyInit(bool tlsInsecure);

#endif  // BRIDGE_BRIDGENOTIFY_H_

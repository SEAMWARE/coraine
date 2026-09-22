#ifndef BRIDGE_BRIDGESAMPLEIN_H_
#define BRIDGE_BRIDGESAMPLEIN_H_

//
// FILE            bridgeSampleIn.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdint.h>                                   // int64_t



// -----------------------------------------------------------------------------
//
// bridgeSampleIn - the broker's side of BridgeBroker::sampleIn
//
// A foreign endpoint produced a value. Resolve it to its Channel and do the
// NGSI-LD work: store the attribute, notify whoever subscribed, record the
// temporal event.
//
// ⚠⚠ CALLED FROM A PLUGIN THREAD - a DDS reader, an MQTT network loop. The
// broker did not create this thread and its thread-locals are not initialised.
// Everything that makes the call safe is here, on the broker's side of the
// seam, and not in any plugin.
//
// Signature matches BridgeBroker::sampleIn exactly; the broker installs it
// there.
//
extern int bridgeSampleIn(const char* bridgeName,
                          const char* endpoint,
                          const char* json,
                          int64_t     publishTime);

#endif  // BRIDGE_BRIDGESAMPLEIN_H_

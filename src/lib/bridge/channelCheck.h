#ifndef BRIDGE_CHANNEL_CHECK_H_
#define BRIDGE_CHANNEL_CHECK_H_

//
// FILE            channelCheck.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The payload check (doc/bridge-channels.md, doc/asset-channels.md § 2.1) - a bridge may refuse what a
// client writes to an attribute one of its Channels, DataChannels or AssetChannels maps, before the broker sends or
// stores any of it (BridgeDriver.payloadCheck, bridge ABI 13).
//
// REFUSE ONLY: the value is written as the client sent it, or that attribute is not written at all.
// OPTIONAL, per bridge: without a loaded bridge that has a check, a write costs one test of a flag.
//
// Not the Channel's owner rule (who writes) and not the send step (transport first): its own function,
// its own counter (payloadsRefused) and its own error. What it shares with the send step is the
// request's list of attributes not written (AttrsNotWritten), which each route reads once.
//
// Compiled in with COR_FEATURE_CHANNEL_CHECKS.
//
#include <stdbool.h>                                  // bool

#include "corTree/CorNode.h"                          // CorNode
#include "db/Tenant.h"                                // Tenant
#include "serviceRoutines/AttrsNotWritten.h"          // AttrsNotWritten



// -----------------------------------------------------------------------------
//
// channelChecksActive - is any loaded bridge one with a payload check?
//
extern bool channelChecksActive(void);



// -----------------------------------------------------------------------------
//
// channelChecksBeforeWrite - each attribute of the fragment a checking bridge's channel maps, checked
//
// Called by every write handler before bridgeRequestsBeforeWrite, with the same fragment and flags: a
// refused value is neither sent nor stored. The value checked is the default instance's, as the client
// wrote it (on a merge route, the fragment - not the result of the merge).
//
// A refusal is 400 (the bridge's reason); a check that FAILS is a refusal too, 503 (the bridge could not
// check the value). ONE attribute: the request fails and nothing is written. Several, or a batch
// (BRIDGE_REQ_PER_ENTITY): only that attribute is taken out of the fragment and added to notWrittenP -
// the other attributes, and the other entities, go on.
//
// @return false, with the error set, when the request fails as a whole.
//
extern bool channelChecksBeforeWrite(Tenant* tenantP, const char* entityId, CorNode* fragmentP, int flags, AttrsNotWritten* notWrittenP);

#endif  // BRIDGE_CHANNEL_CHECK_H_

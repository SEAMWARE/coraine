#ifndef SRC_LIB_BRIDGE_BRIDGECORETERMS_H_
#define SRC_LIB_BRIDGE_BRIDGECORETERMS_H_

//
// FILE            bridgeCoreTerms.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "kalloc/KAlloc.h"                            // KAlloc



// -----------------------------------------------------------------------------
//
// bridgeCoreTermsAdd - the terms of the ContextBridge / Channel / Goal payloads, made core terms
//
// These payloads are proposed to ETSI for the NGSI-LD standard, and their terms
// would go into the core @context - so they are core terms here already: never
// expanded, never overridden by a user @context. Called once, right after
// corLdInit.
//
// ⚠ The core @context overrides EVERY other context. Each name was checked
// absent from Smart Data Models, schema.org, SOSA/SSN and SAREF: the design's
// Bridge, bridge, target, direction and result were all taken, hence
// ContextBridge, bridgeId, channelTarget, channelDirection, goalResult.
// ContextBridge is provisional.
//
extern int bridgeCoreTermsAdd(KAlloc* kaP);

#endif  // SRC_LIB_BRIDGE_BRIDGECORETERMS_H_

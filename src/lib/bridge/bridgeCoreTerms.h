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
// ⏳ TEMPORARY - until a published core context carries these terms. ETSI does
// one or two releases a year and this feature is big, so expect a year or
// more. Then: delete the table (and this call) rather than keep it beside the
// new core - corLdCoreTermsAdd already leaves alone any term the core defines,
// so until it is deleted it is dead weight, not a conflict. Check the names
// ETSI settles on: any that differ from ours is a rename for our clients.
//
extern int bridgeCoreTermsAdd(KAlloc* kaP);

#endif  // SRC_LIB_BRIDGE_BRIDGECORETERMS_H_

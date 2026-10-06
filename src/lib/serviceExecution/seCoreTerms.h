#ifndef SRC_LIB_SERVICEEXECUTION_SECORETERMS_H_
#define SRC_LIB_SERVICEEXECUTION_SECORETERMS_H_

//
// FILE            seCoreTerms.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corAlloc/CorAlloc.h"                        // CorAlloc



// -----------------------------------------------------------------------------
//
// seCoreTermsAdd - the terms of Service Execution's payloads (doc/service-execution.md), made core terms
//
// Service Execution (ETSI GR CIM-055) is to enter the NGSI-LD API spec, and its terms would go into
// the core @context - so they are core terms here already: never expanded, never overridden by a user
// @context. Called once, right after corLdInit, before ldCoreTermIdsInit (their ids: corNgsild's
// CorTerm.h).
//
// ⚠ The core @context overrides EVERY other context. Each name was checked absent from the NGSI-LD
// core context v1.9, Smart Data Models, schema.org, SAREF and SOSA/SSN. The draft's `input` and
// `output` are Smart Data Models attributes, hence inputSchema / outputSchema (a description's JSON
// Schemas) and executionInput / executionOutput (an execution's values).
//
// ⏳ TEMPORARY - until a published core context carries these terms (as bridgeCoreTerms.c).
//
extern int seCoreTermsAdd(CorAlloc* kaP);

#endif  // SRC_LIB_SERVICEEXECUTION_SECORETERMS_H_

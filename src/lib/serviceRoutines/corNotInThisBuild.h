#ifndef SRC_LIB_SERVICEROUTINES_CORNOTINTHISBUILD_H_
#define SRC_LIB_SERVICEROUTINES_CORNOTINTHISBUILD_H_

//
// FILE            corNotInThisBuild.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                 // bool



// -----------------------------------------------------------------------------
//
// LD_ERROR_NOT_IN_THIS_BUILD - ProblemDetails 'type' for a compiled-out endpoint
//
// NOT an ETSI URI, on purpose. TS 104-176 § 6.3.2 registers eleven error types
// and exactly one of them describes the DEPLOYMENT rather than the request:
// NoMultiTenantSupport (501), reserved for that single capability. Everything
// else an implementation may legitimately omit has to answer
// OperationNotSupported, which is 422 - "your request cannot be performed" -
// and is indistinguishable from a malformed request. See spec-doubt #124,
// which proposes uri.etsi.org/ngsi-ld/errors/NotAvailableInThisDeployment ->
// 501. Until that is registered we carry the same name in OUR namespace rather
// than invent inside ETSI's; the day it lands, this #define is the one place
// that changes.
//
#define LD_ERROR_NOT_IN_THIS_BUILD  "https://coraine.readthedocs.io/errors/NotAvailableInThisDeployment"



// -----------------------------------------------------------------------------
//
// corNotInThisBuild - service routine for every endpoint compiled out of this build
//
// The single handler that the per-feature macros in ngsildServices.c expand to
// when their feature is OFF. Answers 501 with the verb and URL in the detail, so
// a client can tell "this build does not implement that" from "your request was
// wrong" (404) or "that operation cannot be performed" (422).
//
// Referenced ONLY from an #else branch, so a build with every feature ON never
// mentions it.
//
extern bool corNotInThisBuild(void);

#endif  // SRC_LIB_SERVICEROUTINES_CORNOTINTHISBUILD_H_

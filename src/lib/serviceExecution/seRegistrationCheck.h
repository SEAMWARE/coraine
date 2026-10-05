#ifndef SRC_LIB_SERVICEEXECUTION_SEREGISTRATIONCHECK_H_
#define SRC_LIB_SERVICEEXECUTION_SEREGISTRATIONCHECK_H_

//
// FILE            seRegistrationCheck.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                  // bool

#include "corTree/CorNode.h"                          // CorNode



// -----------------------------------------------------------------------------
//
// seRegistrationCheck - is this a Service Registration? false: a 400 has been raised (ldError)
//
// A whole registration - a POST's body, or a PATCH merged into the stored one:
//
//   type                 "ServiceRegistration"
//   id                   a URI - optional (generated)
//   endpoint             a URI - where the service is executed
//   entities             a non-empty array of { type, id | idPattern }
//   q, geoQ              optional - entities matching them only
//   executionTimeout     optional - an ISO 8601 duration
//   serviceInformation   { serviceName, title?, description?, mode?, inputSchema?, outputSchema? }
//                        mode: "synchronous" (the default) or "asynchronous"
//   createdAt, modifiedAt  the broker's - a stored registration has them
//
// Any other member is refused: a typo, or the draft's `input` / `output` (here inputSchema /
// outputSchema), would otherwise be stored and silently mean nothing.
//
extern bool seRegistrationCheck(CorNode* regP);

#endif  // SRC_LIB_SERVICEEXECUTION_SEREGISTRATIONCHECK_H_

//
// FILE            troeNotAvailable.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                  // strcmp

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_OP_NOT_SUPPORTED

#include "troe/TroeDriver.h"                         // troe
#include "troe/troeNotAvailable.h"                   // Own interface



void troeNotAvailable(const char* op)
{
  const char* alias = (troe.alias != NULL) ? troe.alias : "none";

  // Two flavors:
  //  - no TRoE wired in at all (alias is NULL or "none") → "started without TRoE support"
  //  - a real plugin is active but doesn't implement this op → "plugin doesn't support op"
  // 422 in either case. From the client's view this IS "not implemented by this
  // build", which reads like 501 - but TS 104-176 § 6.3.2 maps each error type to
  // one status, and OperationNotSupported is 422. The only 501 in that table is
  // NoMultiTenantSupport, a type reserved for one specific capability, so there
  // is nothing registered that says "this deployment lacks an optional
  // component" in general. See spec-doubts-2 #124; until it moves, the detail
  // carries what the status cannot.
  if (strcmp(alias, "none") == 0)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "TRoE Not Available",
            "broker started without TRoE support; '%s' requires a TRoE plugin", op);
  }
  else
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "TRoE Not Available",
            "TRoE plugin '%s' does not implement '%s'", alias, op);
  }
}

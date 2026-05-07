//
// FILE            troeNotAvailable.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <string.h>                                  // strcmp

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_OP_NOT_SUPPORTED

#include "troe/TroeDriver.h"                         // troe
#include "troe/troeNotAvailable.h"                   // Own interface



void troeNotAvailable(const char* op)
{
  const char* alias = (troe.alias != NULL) ? troe.alias : "none";

  // Two flavors:
  //  - no TRoE wired in at all (alias is NULL or "none") → "started without TRoE support"
  //  - a real plugin is active but doesn't implement this op → "plugin doesn't support op"
  // 501 in either case: from the client's view the operation is not implemented
  // by this server build/configuration.
  if (strcmp(alias, "none") == 0)
  {
    ldError(501, LD_ERROR_OP_NOT_SUPPORTED, "TRoE Not Available",
            "broker started without TRoE support; '%s' requires a TRoE plugin", op);
  }
  else
  {
    ldError(501, LD_ERROR_OP_NOT_SUPPORTED, "TRoE Not Available",
            "TRoE plugin '%s' does not implement '%s'", alias, op);
  }
}

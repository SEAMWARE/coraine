//
// FILE            adminTroeDump.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /admin/troe/dump — return events captured by a dev/test TRoE plugin
// (today: troeRamdb). Production plugins (timescale, parquet) leave the
// dumpInfo slot NULL → 501.
//
#include <stddef.h>                               // NULL

#include "corAlloc/CorAlloc.h"                    // CorAlloc
#include "corTree/corTreeBuilder.h"               // corTreeObject, corTreeChildAdd
#include "corRest/CorRestState.h"                   // corRest
#include "corNgsild/corNgsild.h"                    // ldError, LD_ERROR_*

#include "troe/TroeDriver.h"                      // troe

#include "api/admin/adminTroeDump.h"              // Own interface



bool adminGetTroeDump(void)
{
  if (troe.dumpInfo == NULL)
  {
    ldError(422, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported",
            "Not Implemented",
            "active TRoE plugin does not support dump (use --troe=corDB)");
    return true;
  }

  CorAlloc* allocP = corRest.kallocP;
  CorNode* root  = corTreeObject(allocP, NULL);

  troe.dumpInfo(&corRest.kalloc, root);

  corRest.out.responseTree = root;
  return true;
}

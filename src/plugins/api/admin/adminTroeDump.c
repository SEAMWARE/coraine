//
// FILE            adminTroeDump.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /admin/troe/dump — return events captured by a dev/test TRoE plugin
// (today: troeRamdb). Production plugins (timescale, parquet) leave the
// dumpInfo slot NULL → 501.
//
#include <stddef.h>                               // NULL

#include "kjson/kjBuilder.h"                      // kjObject, kjChildAdd
#include "swRest/SwRestState.h"                   // swRest
#include "swNgsild/swNgsild.h"                    // ldError, LD_ERROR_*

#include "troe/TroeDriver.h"                      // troe

#include "api/admin/adminTroeDump.h"              // Own interface



bool adminGetTroeDump(void)
{
  if (troe.dumpInfo == NULL)
  {
    ldError(422, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported",
            "Not Implemented",
            "active TRoE plugin does not support dump (use --troe=ramdb)");
    return true;
  }

  Kjson*  kjsonP = swRest.kjsonP;
  KjNode* root   = kjObject(kjsonP, NULL);

  troe.dumpInfo(&swRest.kalloc, root);

  swRest.out.responseTree = root;
  return true;
}

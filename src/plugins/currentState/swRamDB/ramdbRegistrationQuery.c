//
// FILE            ramdbRegistrationQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stddef.h>                                   // NULL

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjBuilder.h"                          // kjArray, kjChildAdd
#include "swRest/SwRestState.h"                       // swRest

#include "db/DbDriver.h"                              // DB_OK, Tenant
#include "currentState/swRamDB/ramdbStore.h"          // ramdbRegistrations
#include "currentState/swRamDB/ramdbRegistrationQuery.h"  // Own interface



// -----------------------------------------------------------------------------
//
// ramdbRegistrationQuery -
//
int ramdbRegistrationQuery(Tenant* tenantP, int limit, int offset, KjNode** arrayPP)
{
  KjNode* registrations = ramdbRegistrations(tenantP);
  // Request-arena array (freed after use), matching mongoc — a NULL (malloc)
  // array would leak its container on every cache-load.
  KjNode* resultArray   = kjArray(swRest.kjsonP, NULL);

  int ix    = 0;
  int added = 0;

  for (KjNode* rP = registrations->value.firstChildP; rP != NULL; rP = rP->next)
  {
    if (ix < offset)
    {
      ix++;
      continue;
    }

    if (limit > 0 && added >= limit)
      break;

    KjNode* cloneP = kjClone(swRest.kjsonP, rP);
    kjChildAdd(resultArray, cloneP);
    added++;
    ix++;
  }

  *arrayPP = resultArray;
  return DB_OK;
}

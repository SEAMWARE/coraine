//
// FILE            corDbRegistrationQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                   // NULL

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjBuilder.h"                          // kjArray, kjChildAdd
#include "corRest/CorRestState.h"                       // corRest

#include "db/DbDriver.h"                              // DB_OK, Tenant
#include "currentState/corDB/corDbStore.h"          // corDbRegistrations
#include "currentState/corDB/corDbRegistrationQuery.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbRegistrationQuery -
//
int corDbRegistrationQuery(Tenant* tenantP, int limit, int offset, KjNode** arrayPP)
{
  KjNode* registrations = corDbRegistrations(tenantP);
  // Request-arena array (freed after use), matching mongoc — a NULL (malloc)
  // array would leak its container on every cache-load.
  KjNode* resultArray   = kjArray(corRest.kjsonP, NULL);

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

    KjNode* cloneP = kjClone(corRest.kjsonP, rP);
    kjChildAdd(resultArray, cloneP);
    added++;
    ix++;
  }

  *arrayPP = resultArray;
  return DB_OK;
}

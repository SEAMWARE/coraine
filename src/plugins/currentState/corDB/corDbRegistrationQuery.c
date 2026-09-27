//
// FILE            corDbRegistrationQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                   // NULL

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeBuilder.h"                   // corTreeArray, corTreeChildAdd
#include "corRest/CorRestState.h"                       // corRest

#include "db/DbDriver.h"                              // DB_OK, Tenant
#include "currentState/corDB/corDbStore.h"          // corDbRegistrations
#include "currentState/corDB/corDbRegistrationQuery.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbRegistrationQuery -
//
int corDbRegistrationQuery(Tenant* tenantP, int limit, int offset, CorNode** arrayPP)
{
  COR_DB_READ(tenantP);

  CorNode* registrations = corDbRegistrations(tenantP);
  // Request-arena array (freed after use), matching mongoc — a NULL (malloc)
  // array would leak its container on every cache-load.
  CorNode* resultArray  = corTreeArray(corRest.kallocP, NULL);

  int ix    = 0;
  int added = 0;

  for (CorNode* rP = registrations->value.head; rP != NULL; rP = rP->next)
  {
    if (ix < offset)
    {
      ix++;
      continue;
    }

    if (limit > 0 && added >= limit)
      break;

    CorNode* cloneP = corTreeClone(corRest.kallocP, rP);
    corTreeChildAdd(resultArray, cloneP);
    added++;
    ix++;
  }

  *arrayPP = resultArray;
  return DB_OK;
}

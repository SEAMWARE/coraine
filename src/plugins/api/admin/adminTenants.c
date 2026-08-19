//
// FILE            adminTenants.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                               // NULL

#include "kjson/kjBuilder.h"                      // kjArray, kjString, kjChildAdd
#include "corRest/CorRestState.h"                   // corRest

#include "db/Tenant.h"                            // Tenant, tenantList

#include "api/admin/adminTenants.h"               // Own interface



// -----------------------------------------------------------------------------
//
// adminGetTenants -
//
// Returns a JSON array of tenant names (strings).
// The default tenant is not included.
//
bool adminGetTenants(void)
{
  Kjson*  kjsonP = corRest.kjsonP;
  KjNode* root   = kjArray(kjsonP, NULL);

  for (Tenant* tP = tenantList; tP != NULL; tP = tP->next)
    kjChildAdd(root, kjString(kjsonP, NULL, tP->name));

  corRest.out.responseTree = root;
  return true;
}

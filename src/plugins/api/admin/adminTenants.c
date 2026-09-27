//
// FILE            adminTenants.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                               // NULL

#include "corAlloc/CorAlloc.h"                    // CorAlloc
#include "corTree/corTreeBuilder.h"               // corTreeArray, corTreeString, corTreeChildAdd
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
  CorAlloc* allocP = corRest.kallocP;
  CorNode* root  = corTreeArray(allocP, NULL);

  for (Tenant* tP = tenantList; tP != NULL; tP = tP->next)
    corTreeChildAdd(root, corTreeString(allocP, NULL, tP->name));

  corRest.out.responseTree = root;
  return true;
}

//
// FILE            adminTenants.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stddef.h>                               // NULL

#include "kjson/kjBuilder.h"                      // kjArray, kjString, kjChildAdd
#include "swRest/SwRestState.h"                   // swRest

#include "db/Tenant.h"                            // Tenant, tenantList

#include "api/admin/adminTenants.h"               // Own interface



// -----------------------------------------------------------------------------
//
// adminGetTenants -
//
// Returns a JSON array of tenant names (strings).
// The default tenant is not included (same as Orion-LD).
//
bool adminGetTenants(void)
{
  Kjson*  kjsonP = swRest.kjsonP;
  KjNode* root   = kjArray(kjsonP, NULL);

  for (Tenant* tP = tenantList; tP != NULL; tP = tP->next)
    kjChildAdd(root, kjString(kjsonP, NULL, tP->name));

  swRest.out.responseTree = root;
  return true;
}

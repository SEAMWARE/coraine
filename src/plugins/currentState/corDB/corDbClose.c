//
// FILE            corDbClose.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "ktrace/kTrace.h"                               // KT_I
#include "kjson/kjFree.h"                                // kjFree

#include "db/Tenant.h"                                   // tenant0, tenantList
#include "currentState/corDB/corDbGeoMatch.h"          // corDbGeoClose
#include "currentState/corDB/corDbClose.h"             // Own interface



// -----------------------------------------------------------------------------
//
// corDbFreeTenantStore - free the per-tenant KjNode tree
//
static void corDbFreeTenantStore(Tenant* tenantP)
{
  if (tenantP->pluginData != NULL)
  {
    kjFree(tenantP->pluginData);
    tenantP->pluginData = NULL;
  }
}



// -----------------------------------------------------------------------------
//
// corDbClose -
//
void corDbClose(void)
{
  corDbFreeTenantStore(&tenant0);

  for (Tenant* tP = tenantList; tP != NULL; tP = tP->next)
    corDbFreeTenantStore(tP);

  corDbGeoClose();
  KT_I("corDB: closed (all tenant stores freed)");
}

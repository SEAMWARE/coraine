//
// FILE            ramdbClose.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "ktrace/kTrace.h"                               // KT_I
#include "kjson/kjFree.h"                                // kjFree

#include "db/Tenant.h"                                   // tenant0, tenantList
#include "currentState/corRamDB/ramdbGeoMatch.h"          // ramdbGeoClose
#include "currentState/corRamDB/ramdbClose.h"             // Own interface



// -----------------------------------------------------------------------------
//
// ramdbFreeTenantStore - free the per-tenant KjNode tree
//
static void ramdbFreeTenantStore(Tenant* tenantP)
{
  if (tenantP->pluginData != NULL)
  {
    kjFree(tenantP->pluginData);
    tenantP->pluginData = NULL;
  }
}



// -----------------------------------------------------------------------------
//
// ramdbClose -
//
void ramdbClose(void)
{
  ramdbFreeTenantStore(&tenant0);

  for (Tenant* tP = tenantList; tP != NULL; tP = tP->next)
    ramdbFreeTenantStore(tP);

  ramdbGeoClose();
  KT_I("corRamDB: closed (all tenant stores freed)");
}

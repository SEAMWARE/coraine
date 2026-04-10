//
// FILE            ramdbInit.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "ktrace/kTrace.h"                               // KT_I

#include "db/Tenant.h"                                   // tenant0
#include "currentState/swRamDB/ramdbStore.h"             // ramdbTenantStore
#include "currentState/swRamDB/ramdbGeoMatch.h"          // ramdbGeoInit
#include "currentState/swRamDB/ramdbInit.h"              // Own interface



// -----------------------------------------------------------------------------
//
// ramdbInit -
//
int ramdbInit(void)
{
  //
  // Create the store for the default tenant eagerly
  //
  ramdbTenantStore(&tenant0);
  ramdbGeoInit();

  KT_I("swRamDB: in-memory store ready (per-tenant KjNode trees, GEOS enabled)");
  return 0;
}

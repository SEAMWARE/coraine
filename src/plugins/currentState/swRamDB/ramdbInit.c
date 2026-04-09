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

  KT_I("swRamDB: in-memory store ready (per-tenant KjNode trees)");
  return 0;
}

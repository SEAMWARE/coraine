//
// FILE            corDbInit.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "ktrace/kTrace.h"                               // KT_I

#include "db/Tenant.h"                                   // tenant0
#include "currentState/corDB/corDbStore.h"             // corDbTenantStore
#include "currentState/corDB/corDbGeoMatch.h"          // corDbGeoInit
#include "currentState/corDB/corDbInit.h"              // Own interface



// -----------------------------------------------------------------------------
//
// corDbInit -
//
int corDbInit(void)
{
  //
  // Create the store for the default tenant eagerly
  //
  corDbTenantStore(&tenant0);
  corDbGeoInit();

  KT_I("corDB: in-memory store ready (per-tenant KjNode trees, GEOS enabled)");
  return 0;
}

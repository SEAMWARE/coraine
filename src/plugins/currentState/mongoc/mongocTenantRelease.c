//
// FILE            mongocTenantRelease.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "db/DbDriver.h"                                 // DB_OK
#include "currentState/mongoc/mongocGeoIndex.h"          // mongocGeoIndexCacheRelease
#include "currentState/mongoc/mongocTenantRelease.h"     // Own interface



// -----------------------------------------------------------------------------
//
// mongocTenantRelease -
//
int mongocTenantRelease(Tenant* tenantP)
{
  mongocGeoIndexCacheRelease(tenantP);
  return DB_OK;
}

//
// FILE            mongocTenantDrop.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Drop a tenant's entire database. Currently only used by the
// snapshot subsystem to reclaim per-snapshot tenant storage on
// snapshot delete / purge.
//
#include <mongoc/mongoc.h>                           // mongoc_database_t

#include "ktrace/kTrace.h"                           // KT_E

#include "db/DbDriver.h"                             // DB_OK, DB_ERR
#include "currentState/mongoc/mongocTenantDrop.h"    // Own interface


extern mongoc_client_pool_t* poolP;


int mongocTenantDrop(Tenant* tenantP)
{
  if (tenantP == NULL || tenantP->dbName[0] == 0)
    return DB_ERR;

  mongoc_client_t*    clientP = mongoc_client_pool_pop(poolP);
  mongoc_database_t*  dbP     = mongoc_client_get_database(clientP, tenantP->dbName);

  bson_error_t error;
  bool ok = mongoc_database_drop(dbP, &error);

  mongoc_database_destroy(dbP);
  mongoc_client_pool_push(poolP, clientP);

  if (!ok)
  {
    KT_E("mongoc: tenantDrop failed: %s", error.message);
    return DB_ERR;
  }
  return DB_OK;
}

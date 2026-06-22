//
// FILE            mongocTenantSetup.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <mongoc/mongoc.h>                           // mongoc_client_pool_t, ...

#include "ktrace/kTrace.h"                               // KT_I, KT_E, KT_V

#include "db/Tenant.h"                               // Tenant
#include "currentState/mongoc/mongocGeoIndex.h"                   // mongocGeoIndexInit
#include "currentState/mongoc/mongocTenantSetup.h"                // Own interface



// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocTenantSetup - create indexes (type + geo) for a tenant's database
//
int mongocTenantSetup(Tenant* tenantP)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  //
  // Ensure index on "type" for entity queries
  //
  bson_t        keys;
  bson_error_t  error;

  bson_init(&keys);
  BSON_APPEND_INT32(&keys, "type", 1);

  mongoc_index_model_t* indexModelP = mongoc_index_model_new(&keys, NULL);

  if (mongoc_collection_create_indexes_with_opts(collP, &indexModelP, 1, NULL, NULL, &error))
    KT_V("mongoc: ensured index on 'type' for db '%s'", tenantP->dbName);
  else
    KT_E("mongoc: failed to create index on 'type' for db '%s': %s", tenantP->dbName, error.message);

  mongoc_index_model_destroy(indexModelP);
  bson_destroy(&keys);

  //
  // Compound index {createdAt, _id} backing the default query sort (createdAt
  // with _id as the unique tiebreak — see mongocEntityQuery). Without it that
  // sort is an in-memory sort: expensive, and MongoDB hard-caps an unindexed
  // sort at 32 MB, which a large result set would exceed.
  //
  bson_t cKeys;
  bson_init(&cKeys);
  BSON_APPEND_INT32(&cKeys, "createdAt", 1);
  BSON_APPEND_INT32(&cKeys, "_id", 1);

  mongoc_index_model_t* cIndexModelP = mongoc_index_model_new(&cKeys, NULL);

  if (mongoc_collection_create_indexes_with_opts(collP, &cIndexModelP, 1, NULL, NULL, &error))
    KT_V("mongoc: ensured index on {createdAt,_id} for db '%s'", tenantP->dbName);
  else
    KT_E("mongoc: failed to create {createdAt,_id} index for db '%s': %s", tenantP->dbName, error.message);

  mongoc_index_model_destroy(cIndexModelP);
  bson_destroy(&cKeys);

  //
  // Scan existing entities for GeoProperty attributes and create 2dsphere indexes
  //
  mongocGeoIndexInit(tenantP, collP);

  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return 0;
}

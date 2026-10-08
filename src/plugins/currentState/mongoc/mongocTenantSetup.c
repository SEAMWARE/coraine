//
// FILE            mongocTenantSetup.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <mongoc/mongoc.h>                           // mongoc_client_pool_t, ...

#include "corLog/corLog.h"                               // COR_I, COR_E, COR_V

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
// mongocTenantSetup - create indexes ({type,createdAt,_id}, {createdAt,_id}, geo) for a tenant's database
//
int mongocTenantSetup(Tenant* tenantP)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  bson_error_t  error;

  //
  // Compound index {type, createdAt, _id} for entity queries by type, in the default order
  //
  // The default order of a query is {createdAt: 1, _id: 1} (mongocEntityQuery). With an index on
  // {type} alone, MongoDB has two plans for `type=X` in that order, and both read more than the page
  // (explain(), 100 000 entities, MongoDB 8.2): for a common type it walks the {createdAt,_id} index
  // below and filters on the type - 191 documents for a page of 20 of a type that is 10 % of them; for
  // a rarer one it reads EVERY entity of the type through {type} and sorts them in memory - 1 000 for
  // any page of a 1 % type, and a type of millions would hit the in-memory sort's limit. With the type
  // first in the order's index it reads exactly the page: equality on the type, then the order straight
  // from the index (perfRun's rare-type scenario: 19.7x the requests/s for a page of 20). An entity
  // with several types (an array) is a multikey entry per type, served the same.
  //
  bson_t keys;

  bson_init(&keys);
  BSON_APPEND_INT32(&keys, "type",      1);
  BSON_APPEND_INT32(&keys, "createdAt", 1);
  BSON_APPEND_INT32(&keys, "_id",       1);

  mongoc_index_model_t* indexModelP = mongoc_index_model_new(&keys, NULL);

  if (mongoc_collection_create_indexes_with_opts(collP, &indexModelP, 1, NULL, NULL, &error))
    COR_V("mongoc: ensured index on {type,createdAt,_id} for db '%s'", tenantP->dbName);
  else
    COR_E("mongoc: failed to create {type,createdAt,_id} index for db '%s': %s", tenantP->dbName, error.message);

  mongoc_index_model_destroy(indexModelP);
  bson_destroy(&keys);

  //
  // The index on {type} alone, as databases created before the one above have it: a prefix of the new
  // index, so it serves nothing the new one does not - and every write pays for it. Dropped by its
  // name; a database without it answers "index not found", which is fine.
  //
  if (mongoc_collection_drop_index_with_opts(collP, "type_1", NULL, &error))
    COR_I("mongoc: dropped the old index on 'type' (type_1) for db '%s'", tenantP->dbName);
  else if (error.code != 27)   // 27: IndexNotFound
    COR_W("mongoc: could not drop the old index 'type_1' for db '%s': %s", tenantP->dbName, error.message);

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
    COR_V("mongoc: ensured index on {createdAt,_id} for db '%s'", tenantP->dbName);
  else
    COR_E("mongoc: failed to create {createdAt,_id} index for db '%s': %s", tenantP->dbName, error.message);

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

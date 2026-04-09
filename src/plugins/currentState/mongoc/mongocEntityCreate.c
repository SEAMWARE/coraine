//
// FILE            mongocEntityCreate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <mongoc/mongoc.h>                           // mongoc_collection_t, mongoc_collection_insert_one

#include "ktrace/kTrace.h"                               // KT_E
#include "kjson/KjNode.h"                            // KjNode

#include "db/DbDriver.h"                             // DB_OK, DB_ALREADY_EXISTS, DB_ERR
#include "currentState/mongoc/mongocKjTreeToBson.h"               // mongocKjTreeToBson
#include "currentState/mongoc/mongocGeoIndex.h"                   // mongocGeoIndexEnsure
#include "currentState/mongoc/mongocEntityCreate.h"               // Own interface



// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocEntityCreate -
//
int mongocEntityCreate(Tenant* tenantP, const char* entityId, KjNode* entityP)
{
  //
  // Convert KjTree to BSON
  //
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");
  bson_t                bson;

  mongocKjTreeToBson(entityP, &bson);

  //
  // Insert
  //
  bson_error_t error;
  bool ok = mongoc_collection_insert_one(collP, &bson, NULL, NULL, &error);

  bson_destroy(&bson);

  if (!ok)
  {
    mongoc_collection_destroy(collP);
    mongoc_client_pool_push(poolP, clientP);

    if (error.code == 11000)
      return DB_ALREADY_EXISTS;

    KT_E("mongoc: entityCreate failed: %s", error.message);
    return DB_ERR;
  }

  //
  // Ensure 2dsphere indexes for any GeoProperty attributes
  //
  mongocGeoIndexEnsure(tenantP, entityP, collP);

  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return DB_OK;
}

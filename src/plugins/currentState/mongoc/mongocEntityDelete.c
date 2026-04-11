//
// FILE            mongocEntityDelete.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <mongoc/mongoc.h>                           // mongoc_collection_t, mongoc_collection_delete_one

#include "ktrace/kTrace.h"                           // KT_E

#include "db/DbDriver.h"                             // DB_OK, DB_NOT_FOUND, DB_ERR
#include "currentState/mongoc/mongocEntityDelete.h"  // Own interface



// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocEntityDelete -
//
int mongocEntityDelete(Tenant* tenantP, const char* entityId)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  //
  // Build filter: { "_id": entityId }
  //
  bson_t filter;
  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", entityId);

  //
  // Delete
  //
  bson_t        reply;
  bson_error_t  error;
  bool          ok = mongoc_collection_delete_one(collP, &filter, NULL, &reply, &error);

  int result = DB_OK;

  if (!ok)
  {
    KT_E("mongoc: entityDelete failed: %s", error.message);
    result = DB_ERR;
  }
  else
  {
    //
    // Check deletedCount in the reply
    //
    bson_iter_t iter;
    if (bson_iter_init_find(&iter, &reply, "deletedCount") && BSON_ITER_HOLDS_INT32(&iter))
    {
      int32_t deleted = bson_iter_int32(&iter);
      if (deleted == 0)
        result = DB_NOT_FOUND;
    }
    else if (bson_iter_init_find(&iter, &reply, "deletedCount") && BSON_ITER_HOLDS_INT64(&iter))
    {
      int64_t deleted = bson_iter_int64(&iter);
      if (deleted == 0)
        result = DB_NOT_FOUND;
    }
  }

  bson_destroy(&reply);
  bson_destroy(&filter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}

//
// FILE            mongocSubscriptionDelete.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <mongoc/mongoc.h>                           // mongoc_collection_t, mongoc_collection_delete_one

#include "ktrace/kTrace.h"                           // KT_E

#include "db/DbDriver.h"                             // DB_OK, DB_NOT_FOUND, DB_ERR
#include "currentState/mongoc/mongocSubscriptionDelete.h"  // Own interface



// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocSubscriptionDelete -
//
int mongocSubscriptionDelete(Tenant* tenantP, const char* subId)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "subscriptions");

  bson_t filter;
  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", subId);

  bson_t        reply;
  bson_error_t  error;
  bool          ok = mongoc_collection_delete_one(collP, &filter, NULL, &reply, &error);

  int result = DB_OK;

  if (!ok)
  {
    KT_E("mongoc: subscriptionDelete failed: %s", error.message);
    result = DB_ERR;
  }
  else
  {
    bson_iter_t iter;
    if (bson_iter_init_find(&iter, &reply, "deletedCount") && BSON_ITER_HOLDS_INT32(&iter))
    {
      if (bson_iter_int32(&iter) == 0)
        result = DB_NOT_FOUND;
    }
    else if (bson_iter_init_find(&iter, &reply, "deletedCount") && BSON_ITER_HOLDS_INT64(&iter))
    {
      if (bson_iter_int64(&iter) == 0)
        result = DB_NOT_FOUND;
    }
  }

  bson_destroy(&reply);
  bson_destroy(&filter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}

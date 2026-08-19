//
// FILE            mongocSnapshotDelete.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Remove a Snapshot's metadata document from the originating tenant's
// "snapshots" collection. Frozen entity bodies live in the per-snapshot
// tenant DB and are reclaimed separately via db.tenantDrop.
//
#include <mongoc/mongoc.h>                           // mongoc_collection_t

#include "ktrace/kTrace.h"                           // KT_E

#include "db/DbDriver.h"                             // DB_OK, DB_NOT_FOUND, DB_ERR
#include "currentState/mongoc/mongocSnapshotDelete.h" // Own interface


extern mongoc_client_pool_t* poolP;


int mongocSnapshotDelete(Tenant* tenantP, const char* snapId)
{
  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "snapshots");

  bson_t filter;
  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", snapId);

  bson_t       reply;
  bson_error_t error;
  bool ok = mongoc_collection_delete_one(collP, &filter, NULL, &reply, &error);

  int result = DB_OK;
  if (!ok)
  {
    KT_E("mongoc: snapshotDelete failed: %s", error.message);
    result = DB_ERR;
  }
  else
  {
    bson_iter_t iter;
    if (bson_iter_init_find(&iter, &reply, "deletedCount"))
    {
      int64_t n = BSON_ITER_HOLDS_INT64(&iter)
                    ? bson_iter_int64(&iter)
                    : (int64_t) bson_iter_int32(&iter);
      if (n == 0) result = DB_NOT_FOUND;
    }
  }

  bson_destroy(&reply);
  bson_destroy(&filter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}

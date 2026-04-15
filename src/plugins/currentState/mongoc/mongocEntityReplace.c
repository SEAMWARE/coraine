//
// FILE            mongocEntityReplace.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <mongoc/mongoc.h>                            // mongoc_*, bson_*

#include "ktrace/kTrace.h"                            // KT_E
#include "kjson/KjNode.h"                             // KjNode
#include "swRest/SwRestState.h"                       // swRest

#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, DB_ERR
#include "currentState/mongoc/mongocKjTreeToBson.h"   // mongocKjTreeToBson
#include "currentState/mongoc/mongocBsonToKjTree.h"   // mongocBsonToKjTree
#include "currentState/mongoc/mongocGeoIndex.h"       // mongocGeoIndexEnsure
#include "currentState/mongoc/mongocEntityReplace.h"  // Own interface



// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocEntityReplace -
//
int mongocEntityReplace(Tenant* tenantP, const char* entityId, KjNode* newEntityP, KjNode** oldEntityPP)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  //
  // Filter: { "_id": entityId }
  //
  bson_t filter;
  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", entityId);

  //
  // Replacement document: the full new entity in BSON form
  //
  bson_t replacement;
  mongocKjTreeToBson(newEntityP, &replacement);

  //
  // find_and_modify: atomic "find by _id, replace with new doc, return pre-image".
  // The default return is the pre-image (without set_new(true) it's the old doc),
  // which is exactly what the caller needs for the notification payload's
  // previousValue fields and for logging / diagnostics.
  //
  mongoc_find_and_modify_opts_t* opts = mongoc_find_and_modify_opts_new();
  mongoc_find_and_modify_opts_set_update(opts, &replacement);

  bson_t       reply;
  bson_error_t error;
  bool         ok = mongoc_collection_find_and_modify_with_opts(collP, &filter, opts, &reply, &error);

  int result = DB_OK;

  if (!ok)
  {
    KT_E("mongoc: entityReplace failed: %s", error.message);
    result = DB_ERR;
  }
  else
  {
    //
    // The server returns { "value": <old_doc> | null, "lastErrorObject": {...}, ... }.
    // A null "value" means no document matched — DB_NOT_FOUND.
    //
    bson_iter_t iter;
    if (bson_iter_init_find(&iter, &reply, "value") && BSON_ITER_HOLDS_DOCUMENT(&iter))
    {
      uint32_t      len;
      const uint8_t* data;
      bson_iter_document(&iter, &len, &data);

      bson_t oldDoc;
      if (bson_init_static(&oldDoc, data, len))
      {
        if (oldEntityPP != NULL)
          *oldEntityPP = mongocBsonToKjTree(&swRest.kalloc, &oldDoc);
      }
      else if (oldEntityPP != NULL)
      {
        *oldEntityPP = NULL;
      }
    }
    else
    {
      result = DB_NOT_FOUND;
      if (oldEntityPP != NULL)
        *oldEntityPP = NULL;
    }
  }

  //
  // Refresh geo indexes for any GeoProperty attributes in the new entity.
  // A replacement may introduce or remove them; only ensure-on-present is done
  // (removal of stale indexes is not tracked anywhere else in the codebase).
  //
  if (result == DB_OK)
    mongocGeoIndexEnsure(tenantP, newEntityP, collP);

  bson_destroy(&reply);
  bson_destroy(&replacement);
  bson_destroy(&filter);
  mongoc_find_and_modify_opts_destroy(opts);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}

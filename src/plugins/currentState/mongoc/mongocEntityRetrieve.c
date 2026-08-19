//
// FILE            mongocEntityRetrieve.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <mongoc/mongoc.h>                           // mongoc_collection_t, mongoc_collection_find_with_opts

#include "ktrace/kTrace.h"                               // KT_E
#include "kjson/KjNode.h"                            // KjNode
#include "corRest/CorRestState.h"                      // corRest

#include "db/DbDriver.h"                             // DB_OK, DB_NOT_FOUND, DB_ERR
#include "currentState/mongoc/mongocBsonToKjTree.h"               // mongocBsonToKjTree
#include "currentState/mongoc/mongocEntityRetrieve.h"             // Own interface



// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocEntityRetrieve -
//
int mongocEntityRetrieve(Tenant* tenantP, const char* entityId, KjNode** entityPP)
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
  // Query
  //
  mongoc_cursor_t* cursorP = mongoc_collection_find_with_opts(collP, &filter, NULL, NULL);

  const bson_t* doc;
  int           result;

  if (mongoc_cursor_next(cursorP, &doc))
  {
    *entityPP = mongocBsonToKjTree(&corRest.kalloc, doc);
    result = DB_OK;
  }
  else
  {
    bson_error_t error;

    if (mongoc_cursor_error(cursorP, &error))
    {
      KT_E("mongoc: entityRetrieve failed: %s", error.message);
      result = DB_ERR;
    }
    else
    {
      result = DB_NOT_FOUND;
    }
  }

  bson_destroy(&filter);
  mongoc_cursor_destroy(cursorP);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}

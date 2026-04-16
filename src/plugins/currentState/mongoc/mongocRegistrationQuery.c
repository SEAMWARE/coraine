//
// FILE            mongocRegistrationQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <mongoc/mongoc.h>                           // mongoc_collection_t, mongoc_collection_find_with_opts

#include "ktrace/kTrace.h"                           // KT_E
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjChildAdd
#include "swRest/SwRestState.h"                      // swRest

#include "db/DbDriver.h"                             // DB_OK, DB_ERR
#include "currentState/mongoc/mongocBsonToKjTree.h"  // mongocBsonToKjTree
#include "currentState/mongoc/mongocRegistrationQuery.h"  // Own interface



extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocRegistrationQuery -
//
int mongocRegistrationQuery(Tenant* tenantP, int limit, int offset, KjNode** arrayPP)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "registrations");

  bson_t opts;
  bson_init(&opts);

  if (offset > 0)
    BSON_APPEND_INT64(&opts, "skip", (int64_t) offset);
  if (limit > 0)
    BSON_APPEND_INT64(&opts, "limit", (int64_t) limit);

  bson_t filter;
  bson_init(&filter);

  mongoc_cursor_t* cursorP = mongoc_collection_find_with_opts(collP, &filter, &opts, NULL);

  KjNode* resultArray = kjArray(NULL, NULL);

  const bson_t* doc;
  while (mongoc_cursor_next(cursorP, &doc))
  {
    KjNode* regP = mongocBsonToKjTree(&swRest.kalloc, doc);
    if (regP != NULL)
      kjChildAdd(resultArray, regP);
  }

  bson_error_t error;
  int result = DB_OK;

  if (mongoc_cursor_error(cursorP, &error))
  {
    KT_E("mongoc: registrationQuery failed: %s", error.message);
    result = DB_ERR;
  }

  *arrayPP = resultArray;

  bson_destroy(&filter);
  bson_destroy(&opts);
  mongoc_cursor_destroy(cursorP);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}

//
// FILE            mongocRegistrationRetrieve.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <mongoc/mongoc.h>                           // mongoc_collection_t, mongoc_collection_find_with_opts

#include "ktrace/kTrace.h"                           // KT_E
#include "kjson/KjNode.h"                            // KjNode
#include "corRest/CorRestState.h"                      // corRest

#include "db/DbDriver.h"                             // DB_OK, DB_NOT_FOUND, DB_ERR
#include "currentState/mongoc/mongocBsonToKjTree.h"  // mongocBsonToKjTree
#include "currentState/mongoc/mongocInjectType.h"    // mongocInjectTypeAfterId
#include "currentState/mongoc/mongocRegistrationRetrieve.h"  // Own interface



extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocRegistrationRetrieve -
//
int mongocRegistrationRetrieve(Tenant* tenantP, const char* regId, KjNode** regPP)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "registrations");

  bson_t filter;
  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", regId);

  mongoc_cursor_t* cursorP = mongoc_collection_find_with_opts(collP, &filter, NULL, NULL);

  const bson_t* doc;
  int           result;

  if (mongoc_cursor_next(cursorP, &doc))
  {
    *regPP = mongocBsonToKjTree(&corRest.kalloc, doc);
    // `type` was stripped at insert — put back the JSON-LD constant.
    mongocInjectTypeAfterId(*regPP, "ContextSourceRegistration");
    result = DB_OK;
  }
  else
  {
    bson_error_t error;

    if (mongoc_cursor_error(cursorP, &error))
    {
      KT_E("mongoc: registrationRetrieve failed: %s", error.message);
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

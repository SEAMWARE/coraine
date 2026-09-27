//
// FILE            mongocSubscriptionQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <mongoc/mongoc.h>                           // mongoc_collection_t, mongoc_collection_find_with_opts

#include "corLog/corLog.h"                           // COR_E
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeBuilder.h"                  // corTreeArray, corTreeChildAdd
#include "corRest/CorRestState.h"                      // corRest

#include "db/DbDriver.h"                             // DB_OK, DB_ERR
#include "currentState/mongoc/mongocBsonToTree.h"    // mongocBsonToTree
#include "currentState/mongoc/mongocInjectType.h"    // mongocInjectTypeAfterId
#include "currentState/mongoc/mongocSubscriptionQuery.h"  // Own interface



// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocSubscriptionQuery -
//
int mongocSubscriptionQuery(Tenant* tenantP, int limit, int offset, CorNode** arrayPP)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "subscriptions");

  //
  // Build options: skip + limit
  //
  bson_t opts;
  bson_init(&opts);

  if (offset > 0)
    BSON_APPEND_INT64(&opts, "skip", (int64_t) offset);
  if (limit > 0)
    BSON_APPEND_INT64(&opts, "limit", (int64_t) limit);

  //
  // Query all subscriptions (empty filter)
  //
  bson_t filter;
  bson_init(&filter);

  mongoc_cursor_t* cursorP = mongoc_collection_find_with_opts(collP, &filter, &opts, NULL);

  CorNode* resultArray = corTreeArray(corRest.kallocP, NULL);

  const bson_t* doc;
  while (mongoc_cursor_next(cursorP, &doc))
  {
    CorNode* subP = mongocBsonToTree(&corRest.kalloc, doc);
    if (subP != NULL)
    {
      mongocInjectTypeAfterId(subP, "Subscription");
      corTreeChildAdd(resultArray, subP);
    }
  }

  bson_error_t error;
  int result = DB_OK;

  if (mongoc_cursor_error(cursorP, &error))
  {
    COR_E("mongoc: subscriptionQuery failed: %s", error.message);
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

//
// FILE            mongocSnapshotQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Enumerate all snapshots persisted in the tenant's "snapshots"
// collection. Used at boot by tenantSnapshotCacheReload.
//
#include <mongoc/mongoc.h>                           // mongoc_collection_t

#include "ktrace/kTrace.h"                           // KT_E
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjChildAdd
#include "swRest/SwRestState.h"                      // swRest

#include "db/DbDriver.h"                             // DB_OK, DB_ERR
#include "currentState/mongoc/mongocBsonToKjTree.h"  // mongocBsonToKjTree
#include "currentState/mongoc/mongocInjectType.h"    // mongocInjectTypeAfterId
#include "currentState/mongoc/mongocSnapshotQuery.h" // Own interface


extern mongoc_client_pool_t* poolP;


int mongocSnapshotQuery(Tenant* tenantP, KjNode** arrayPP)
{
  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "snapshots");

  bson_t filter;
  bson_init(&filter);

  mongoc_cursor_t* cursorP = mongoc_collection_find_with_opts(collP, &filter, NULL, NULL);

  KjNode* resultArray = kjArray(swRest.kjsonP, NULL);

  const bson_t* doc;
  while (mongoc_cursor_next(cursorP, &doc))
  {
    KjNode* snapP = mongocBsonToKjTree(&swRest.kalloc, doc);
    if (snapP != NULL)
    {
      mongocInjectTypeAfterId(snapP, "Snapshot");
      kjChildAdd(resultArray, snapP);
    }
  }

  bson_error_t error;
  int result = DB_OK;

  if (mongoc_cursor_error(cursorP, &error))
  {
    KT_E("mongoc: snapshotQuery failed: %s", error.message);
    result = DB_ERR;
  }

  *arrayPP = resultArray;

  bson_destroy(&filter);
  mongoc_cursor_destroy(cursorP);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}

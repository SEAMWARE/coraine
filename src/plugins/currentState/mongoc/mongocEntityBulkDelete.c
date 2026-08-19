//
// FILE            mongocEntityBulkDelete.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// mongoc bulk delete — two round-trips: a $in find that fetches the
// full pre-delete document for each matched id (so the service can
// fire delete notifications without an extra retrieve), followed by
// one bulk_operation_execute with delete_one per existing id. Ids not
// found in the find never enter the bulk — they are flagged
// DB_NOT_FOUND immediately.
//

#include <string.h>                                      // strcmp
#include <stdio.h>                                       // snprintf

#include <mongoc/mongoc.h>                               // mongoc_*

#include "ktrace/kTrace.h"                               // KT_E
#include "kjson/KjNode.h"                                // KjNode

#include "corRest/CorRestState.h"                          // corRest (kalloc arena)

#include "db/DbDriver.h"                                 // DB_OK, DB_NOT_FOUND, DB_ERR, Tenant

#include "currentState/mongoc/mongocBsonToKjTree.h"      // mongocBsonToKjTree
#include "currentState/mongoc/mongocEntityBulkDelete.h"  // Own interface



extern mongoc_client_pool_t* poolP;



// -----------------------------------------------------------------------------
//
// mongocEntityBulkDelete -
//
int mongocEntityBulkDelete(Tenant* tenantP, const char** idV, int N,
                           int* resultsV, KjNode** snapshotsV)
{
  if (N <= 0)
    return DB_ERR;

  for (int i = 0; i < N; i++)
  {
    resultsV[i]   = DB_NOT_FOUND;
    snapshotsV[i] = NULL;
  }

  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  //
  // Pass 1 — $in find to fetch pre-delete docs for snapshotsV.
  //
  {
    bson_t filter = BSON_INITIALIZER;
    bson_t inDoc;
    BSON_APPEND_DOCUMENT_BEGIN(&filter, "_id", &inDoc);

    bson_t idArr;
    BSON_APPEND_ARRAY_BEGIN(&inDoc, "$in", &idArr);

    for (int i = 0; i < N; i++)
    {
      if (idV[i] == NULL) continue;
      char key[16];
      snprintf(key, sizeof(key), "%d", i);
      BSON_APPEND_UTF8(&idArr, key, idV[i]);
    }

    bson_append_array_end(&inDoc, &idArr);
    bson_append_document_end(&filter, &inDoc);

    mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(collP, &filter, NULL, NULL);

    const bson_t* docP = NULL;
    while (mongoc_cursor_next(cursor, &docP))
    {
      bson_iter_t iter;
      if (!bson_iter_init_find(&iter, docP, "_id") || !BSON_ITER_HOLDS_UTF8(&iter))
        continue;

      const char* foundId = bson_iter_utf8(&iter, NULL);

      for (int i = 0; i < N; i++)
      {
        if (idV[i] != NULL && strcmp(idV[i], foundId) == 0)
        {
          snapshotsV[i] = mongocBsonToKjTree(&corRest.kalloc, docP);
          resultsV[i]   = DB_OK;  // optimistic; downgraded if bulk fails
          break;
        }
      }
    }

    mongoc_cursor_destroy(cursor);
    bson_destroy(&filter);
  }

  //
  // Pass 2 — bulk_write with delete_one for every id that was found.
  //
  mongoc_bulk_operation_t* bulk = NULL;
  int bulkCount = 0;

  for (int i = 0; i < N; i++)
  {
    if (resultsV[i] != DB_OK) continue;

    if (bulk == NULL)
    {
      bson_t bulkOpts = BSON_INITIALIZER;
      BSON_APPEND_BOOL(&bulkOpts, "ordered", false);
      bulk = mongoc_collection_create_bulk_operation_with_opts(collP, &bulkOpts);
      bson_destroy(&bulkOpts);
    }

    bson_t selector = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&selector, "_id", idV[i]);
    mongoc_bulk_operation_remove_one(bulk, &selector);
    bson_destroy(&selector);

    bulkCount++;
  }

  if (bulk != NULL)
  {
    bson_t       reply;
    bson_error_t error;
    bool ok = mongoc_bulk_operation_execute(bulk, &reply, &error) > 0;
    if (!ok)
    {
      KT_E("mongoc: entityBulkDelete execute failed: %s", error.message);
      for (int i = 0; i < N; i++)
        if (resultsV[i] == DB_OK)
          resultsV[i] = DB_ERR;
    }
    bson_destroy(&reply);
    mongoc_bulk_operation_destroy(bulk);
  }

  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  bool anyOk = false;
  for (int k = 0; k < N; k++) if (resultsV[k] == DB_OK) { anyOk = true; break; }
  return anyOk ? DB_OK : DB_ERR;
}

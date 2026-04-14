//
// FILE            mongocSubscriptionUpdate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Subscription update uses JSON Merge Patch semantics:
//   - field with null value → remove
//   - field with non-null value → replace or add
//
// Subscriptions are small documents, so we build a $set/$unset from the
// fragment directly rather than fetching the whole document first.
//

#include <string.h>                                  // strcmp

#include <mongoc/mongoc.h>                           // mongoc_collection_t, mongoc_collection_update_one

#include "ktrace/kTrace.h"                           // KT_E
#include "kjson/KjNode.h"                            // KjNode

#include "db/DbDriver.h"                             // DB_OK, DB_NOT_FOUND, DB_ERR
#include "currentState/mongoc/mongocKjTreeToBson.h"  // mongocKjNodeAppend
#include "currentState/mongoc/mongocDotEscape.h"     // mongocEscapeDotsInKey
#include "currentState/mongoc/mongocSubscriptionUpdate.h"  // Own interface



// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocSubscriptionUpdate -
//
int mongocSubscriptionUpdate(Tenant* tenantP, const char* subId, KjNode* fragmentP)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "subscriptions");

  //
  // Build filter: { "_id": subId }
  //
  bson_t filter;
  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", subId);

  //
  // Build $set / $unset from the fragment
  //
  bson_t update;
  bson_t setDoc;
  bson_t unsetDoc;
  bson_init(&update);
  bson_init(&setDoc);
  bson_init(&unsetDoc);

  bool hasSet   = false;
  bool hasUnset = false;

  for (KjNode* fieldP = fragmentP->value.firstChildP; fieldP != NULL; fieldP = fieldP->next)
  {
    if (fieldP->name == NULL)
      continue;

    // Skip "id" and "type" — not updatable
    if (strcmp(fieldP->name, "id") == 0 || strcmp(fieldP->name, "type") == 0)
      continue;

    const char* key = mongocEscapeDotsInKey(fieldP->name);

    if (fieldP->type == KjNull)
    {
      BSON_APPEND_INT32(&unsetDoc, key, 1);
      hasUnset = true;
    }
    else
    {
      mongocKjNodeAppend(&setDoc, key, fieldP);
      hasSet = true;
    }
  }

  if (hasSet)
    BSON_APPEND_DOCUMENT(&update, "$set", &setDoc);
  if (hasUnset)
    BSON_APPEND_DOCUMENT(&update, "$unset", &unsetDoc);

  //
  // Execute update
  //
  int result = DB_OK;

  if (hasSet || hasUnset)
  {
    bson_t        reply;
    bson_error_t  error;
    bool ok = mongoc_collection_update_one(collP, &filter, &update, NULL, &reply, &error);

    if (!ok)
    {
      KT_E("mongoc: subscriptionUpdate failed: %s", error.message);
      result = DB_ERR;
    }
    else
    {
      // Check matchedCount to detect not-found
      bson_iter_t iter;
      int64_t matched = 0;

      if (bson_iter_init_find(&iter, &reply, "matchedCount"))
      {
        if (BSON_ITER_HOLDS_INT32(&iter))
          matched = bson_iter_int32(&iter);
        else if (BSON_ITER_HOLDS_INT64(&iter))
          matched = bson_iter_int64(&iter);
      }

      if (matched == 0)
        result = DB_NOT_FOUND;
    }

    bson_destroy(&reply);
  }
  else
  {
    // Empty fragment — check if subscription exists
    mongoc_cursor_t* cursorP = mongoc_collection_find_with_opts(collP, &filter, NULL, NULL);
    const bson_t* doc;

    if (!mongoc_cursor_next(cursorP, &doc))
      result = DB_NOT_FOUND;

    mongoc_cursor_destroy(cursorP);
  }

  bson_destroy(&update);
  bson_destroy(&setDoc);
  bson_destroy(&unsetDoc);
  bson_destroy(&filter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}

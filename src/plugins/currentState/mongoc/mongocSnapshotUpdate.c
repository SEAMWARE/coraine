//
// FILE            mongocSnapshotUpdate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Snapshot patch — JSON Merge Patch semantics, parallel to
// mongocSubscriptionUpdate. Used by patchSnapshot for priority/lifetime
// changes (the only mutable members per § 5.16.4 / § 5.2.41), and by
// the future async-exec hook to persist a status transition.
//
#include <string.h>                                  // strcmp

#include <mongoc/mongoc.h>                           // mongoc_collection_t

#include "ktrace/kTrace.h"                           // KT_E
#include "kjson/KjNode.h"                            // KjNode

#include "db/DbDriver.h"                             // DB_OK, DB_NOT_FOUND, DB_ERR
#include "currentState/mongoc/mongocKjTreeToBson.h"  // mongocKjNodeAppend
#include "currentState/mongoc/mongocDotEscape.h"     // mongocEscapeDotsInKey
#include "currentState/mongoc/mongocSnapshotUpdate.h" // Own interface


extern mongoc_client_pool_t* poolP;


int mongocSnapshotUpdate(Tenant* tenantP, const char* snapId, KjNode* fragmentP)
{
  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "snapshots");

  bson_t filter;
  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", snapId);

  bson_t update, setDoc, unsetDoc;
  bson_init(&update);
  bson_init(&setDoc);
  bson_init(&unsetDoc);

  bool hasSet = false, hasUnset = false;

  for (KjNode* fieldP = fragmentP->value.firstChildP; fieldP != NULL; fieldP = fieldP->next)
  {
    if (fieldP->name == NULL) continue;
    if (strcmp(fieldP->name, "id") == 0 || strcmp(fieldP->name, "type") == 0) continue;

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

  if (hasSet)   BSON_APPEND_DOCUMENT(&update, "$set",   &setDoc);
  if (hasUnset) BSON_APPEND_DOCUMENT(&update, "$unset", &unsetDoc);

  int result = DB_OK;

  if (hasSet || hasUnset)
  {
    bson_t       reply;
    bson_error_t error;
    bool ok = mongoc_collection_update_one(collP, &filter, &update, NULL, &reply, &error);

    if (!ok)
    {
      KT_E("mongoc: snapshotUpdate failed: %s", error.message);
      result = DB_ERR;
    }
    else
    {
      bson_iter_t iter;
      int64_t matched = 0;
      if (bson_iter_init_find(&iter, &reply, "matchedCount"))
      {
        if      (BSON_ITER_HOLDS_INT32(&iter)) matched = bson_iter_int32(&iter);
        else if (BSON_ITER_HOLDS_INT64(&iter)) matched = bson_iter_int64(&iter);
      }
      if (matched == 0) result = DB_NOT_FOUND;
    }
    bson_destroy(&reply);
  }

  bson_destroy(&update);
  bson_destroy(&setDoc);
  bson_destroy(&unsetDoc);
  bson_destroy(&filter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}

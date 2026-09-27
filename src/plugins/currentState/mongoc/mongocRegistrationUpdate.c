//
// FILE            mongocRegistrationUpdate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Stores a full registration document, replacing whatever is on record for
// `regId`. The NGSI-LD merge (JSON Merge Patch incl. the urn:ngsi-ld:null
// delete-marker) is resolved by the broker before this is called, so the DB
// plugin is a dumb store — it never interprets NGSI-LD null semantics.
//

#include <mongoc/mongoc.h>                           // mongoc_collection_t, mongoc_collection_replace_one

#include "corLog/corLog.h"                           // COR_E
#include "corTree/CorNode.h"                         // CorNode

#include "db/DbDriver.h"                             // DB_OK, DB_NOT_FOUND, DB_ERR
#include "currentState/mongoc/mongocTreeToBson.h"    // mongocNodeAppend, mongocTreeToBson
#include "currentState/mongoc/mongocInjectType.h"    // mongocStripTypeDecouple, mongocStripTypeRestore
#include "currentState/mongoc/mongocRegistrationUpdate.h"  // Own interface



extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocRegistrationUpdate - replace the stored registration with `regP`
//
int mongocRegistrationUpdate(Tenant* tenantP, const char* regId, CorNode* regP)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "registrations");

  bson_t filter;
  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", regId);

  // `type` is the fixed JSON-LD constant "ContextSourceRegistration" — redundant
  // in DB. Strip around BSON emission, preserving the tree (id -> _id is done by
  // mongocTreeToBson).
  CorNode* typeP    = NULL;
  CorNode* typePrevP = NULL;
  mongocStripTypeDecouple(regP, &typeP, &typePrevP);

  bson_t replacement;
  mongocTreeToBson(regP, &replacement);

  mongocStripTypeRestore(regP, typeP, typePrevP);

  bson_t       reply;
  bson_error_t error;
  bool         ok = mongoc_collection_replace_one(collP, &filter, &replacement, NULL, &reply, &error);

  int result = DB_OK;

  if (!ok)
  {
    COR_E("mongoc: registrationUpdate failed: %s", error.message);
    result = DB_ERR;
  }
  else
  {
    bson_iter_t iter;
    int64_t     matched = 0;

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
  bson_destroy(&replacement);
  bson_destroy(&filter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}

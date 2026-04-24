//
// FILE            mongocRegistrationCreate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <mongoc/mongoc.h>                           // mongoc_collection_t, mongoc_collection_insert_one

#include "ktrace/kTrace.h"                           // KT_E
#include "kjson/KjNode.h"                            // KjNode

#include "db/DbDriver.h"                             // DB_OK, DB_ALREADY_EXISTS, DB_ERR
#include "currentState/mongoc/mongocKjTreeToBson.h"  // mongocKjTreeToBson
#include "currentState/mongoc/mongocInjectType.h"    // mongocStripTypeDecouple, mongocStripTypeRestore
#include "currentState/mongoc/mongocRegistrationCreate.h"  // Own interface



extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocRegistrationCreate -
//
int mongocRegistrationCreate(Tenant* tenantP, const char* regId, KjNode* regP)
{
  (void) regId;

  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "registrations");
  bson_t                bson;

  // `type` is the fixed JSON-LD constant "ContextSourceRegistration" —
  // redundant in DB. Strip around BSON emission, preserving the tree.
  KjNode* typeP     = NULL;
  KjNode* typePrevP = NULL;
  mongocStripTypeDecouple(regP, &typeP, &typePrevP);

  mongocKjTreeToBson(regP, &bson);

  mongocStripTypeRestore(regP, typeP, typePrevP);

  bson_error_t error;
  bool ok = mongoc_collection_insert_one(collP, &bson, NULL, NULL, &error);

  bson_destroy(&bson);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  if (!ok)
  {
    if (error.code == 11000)
      return DB_ALREADY_EXISTS;

    KT_E("mongoc: registrationCreate failed: %s", error.message);
    return DB_ERR;
  }

  return DB_OK;
}

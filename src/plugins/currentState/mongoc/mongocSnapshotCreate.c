//
// FILE            mongocSnapshotCreate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Persist a Snapshot's metadata into the originating tenant's
// "snapshots" collection. The persisted tree carries the same fields
// as the cache item's tree, plus the hidden "_snapSeq" added by the
// service routine before calling here (used at boot to rebuild the
// per-snapshot tenant DB name).
//
#include <mongoc/mongoc.h>                           // mongoc_collection_t

#include "ktrace/kTrace.h"                           // KT_E
#include "kjson/KjNode.h"                            // KjNode

#include "db/DbDriver.h"                             // DB_OK, DB_ALREADY_EXISTS, DB_ERR
#include "currentState/mongoc/mongocKjTreeToBson.h"  // mongocKjTreeToBson
#include "currentState/mongoc/mongocInjectType.h"    // mongocStripTypeDecouple, mongocStripTypeRestore
#include "currentState/mongoc/mongocSnapshotCreate.h" // Own interface


extern mongoc_client_pool_t* poolP;


int mongocSnapshotCreate(Tenant* tenantP, const char* snapId, KjNode* snapP)
{
  (void) snapId;  // _id comes from the tree's "id" via mongocKjTreeToBson
  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "snapshots");
  bson_t bson;

  // `type` is the fixed "Snapshot" constant — strip around BSON emission.
  KjNode* typeP     = NULL;
  KjNode* typePrevP = NULL;
  mongocStripTypeDecouple(snapP, &typeP, &typePrevP);

  mongocKjTreeToBson(snapP, &bson);

  mongocStripTypeRestore(snapP, typeP, typePrevP);

  bson_error_t error;
  bool ok = mongoc_collection_insert_one(collP, &bson, NULL, NULL, &error);

  bson_destroy(&bson);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  if (!ok)
  {
    if (error.code == 11000)
      return DB_ALREADY_EXISTS;
    KT_E("mongoc: snapshotCreate failed: %s", error.message);
    return DB_ERR;
  }
  return DB_OK;
}

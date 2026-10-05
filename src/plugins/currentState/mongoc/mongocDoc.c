//
// FILE            mongocDoc.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                  // strcmp

#include <mongoc/mongoc.h>                           // mongoc_*

#include "corLog/corLog.h"                           // COR_E
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeBuilder.h"                  // corTreeArray, corTreeChildAdd
#include "corRest/CorRestState.h"                    // corRest

#include "db/DbDriver.h"                             // DB_OK, DB_ALREADY_EXISTS, DB_NOT_FOUND, DB_ERR
#include "currentState/mongoc/mongocTreeToBson.h"    // mongocNodeAppend
#include "currentState/mongoc/mongocBsonToTree.h"    // mongocBsonToTree
#include "currentState/mongoc/mongocDoc.h"           // Own interface



extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// docToBson - the document, its id as _id, whatever its "id" member says
//
static void docToBson(const char* docId, CorNode* docP, bson_t* bsonP)
{
  bson_init(bsonP);
  BSON_APPEND_UTF8(bsonP, "_id", docId);

  for (CorNode* childP = docP->value.head; childP != NULL; childP = childP->next)
  {
    if ((childP->name != NULL) && (strcmp(childP->name, "id") == 0))
      continue;

    mongocNodeAppend(bsonP, childP->name, childP);
  }
}



// -----------------------------------------------------------------------------
//
// mongocDocCreate -
//
int mongocDocCreate(Tenant* tenantP, const char* collection, const char* docId, CorNode* docP)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, collection);
  bson_t                bson;
  bson_error_t          error;

  docToBson(docId, docP, &bson);

  bool ok = mongoc_collection_insert_one(collP, &bson, NULL, NULL, &error);

  bson_destroy(&bson);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  if (ok == false)
  {
    if (error.code == 11000)
      return DB_ALREADY_EXISTS;

    COR_E("mongoc: docCreate (%s) failed: %s", collection, error.message);
    return DB_ERR;
  }

  return DB_OK;
}



// -----------------------------------------------------------------------------
//
// mongocDocRetrieve -
//
int mongocDocRetrieve(Tenant* tenantP, const char* collection, const char* docId, CorNode** docPP)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, collection);
  bson_t                filter;
  const bson_t*         doc;
  int                   result  = DB_NOT_FOUND;

  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", docId);

  mongoc_cursor_t* cursorP = mongoc_collection_find_with_opts(collP, &filter, NULL, NULL);

  if (mongoc_cursor_next(cursorP, &doc))
  {
    *docPP = mongocBsonToTree(&corRest.kalloc, doc);  // _id back to id
    result = (*docPP != NULL) ? DB_OK : DB_ERR;
  }
  else
  {
    bson_error_t error;

    if (mongoc_cursor_error(cursorP, &error))
    {
      COR_E("mongoc: docRetrieve (%s) failed: %s", collection, error.message);
      result = DB_ERR;
    }
  }

  bson_destroy(&filter);
  mongoc_cursor_destroy(cursorP);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}



// -----------------------------------------------------------------------------
//
// mongocDocQuery -
//
int mongocDocQuery(Tenant* tenantP, const char* collection, CorNode** arrayPP)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, collection);
  bson_t                filter;
  const bson_t*         doc;
  bson_error_t          error;
  int                   result  = DB_OK;
  CorNode*              arrayP  = corTreeArray(corRest.kallocP, NULL);

  bson_init(&filter);

  mongoc_cursor_t* cursorP = mongoc_collection_find_with_opts(collP, &filter, NULL, NULL);

  while (mongoc_cursor_next(cursorP, &doc))
  {
    CorNode* docP = mongocBsonToTree(&corRest.kalloc, doc);

    if (docP != NULL)
      corTreeChildAdd(arrayP, docP);
  }

  if (mongoc_cursor_error(cursorP, &error))
  {
    COR_E("mongoc: docQuery (%s) failed: %s", collection, error.message);
    result = DB_ERR;
  }

  *arrayPP = arrayP;

  bson_destroy(&filter);
  mongoc_cursor_destroy(cursorP);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}



// -----------------------------------------------------------------------------
//
// mongocDocReplace -
//
int mongocDocReplace(Tenant* tenantP, const char* collection, const char* docId, CorNode* docP)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, collection);
  bson_t                filter;
  bson_t                bson;
  bson_t                reply;
  bson_error_t          error;
  int                   result  = DB_OK;

  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", docId);
  docToBson(docId, docP, &bson);

  if (mongoc_collection_replace_one(collP, &filter, &bson, NULL, &reply, &error) == false)
  {
    COR_E("mongoc: docReplace (%s) failed: %s", collection, error.message);
    result = DB_ERR;
  }
  else
  {
    bson_iter_t it;

    if (bson_iter_init_find(&it, &reply, "matchedCount") && (bson_iter_as_int64(&it) == 0))
      result = DB_NOT_FOUND;
  }

  bson_destroy(&reply);
  bson_destroy(&bson);
  bson_destroy(&filter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}



// -----------------------------------------------------------------------------
//
// mongocDocDelete -
//
int mongocDocDelete(Tenant* tenantP, const char* collection, const char* docId)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, collection);
  bson_t                filter;
  bson_t                reply;
  bson_error_t          error;
  int                   result  = DB_OK;

  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", docId);

  if (mongoc_collection_delete_one(collP, &filter, NULL, &reply, &error) == false)
  {
    COR_E("mongoc: docDelete (%s) failed: %s", collection, error.message);
    result = DB_ERR;
  }
  else
  {
    bson_iter_t it;

    if (bson_iter_init_find(&it, &reply, "deletedCount") && (bson_iter_as_int64(&it) == 0))
      result = DB_NOT_FOUND;
  }

  bson_destroy(&reply);
  bson_destroy(&filter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}

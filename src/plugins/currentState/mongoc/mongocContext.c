//
// FILE            mongocContext.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// JSON-LD context persistence — fixed DB "swBroker", collection "contexts".
// Document shape:
//     {
//       "_id":  "<context id>",
//       "url":  "<download url or null>",
//       "kind": <DB_CONTEXT_KIND_*>,
//       "body": "<raw JSON @context document>"
//     }
//

#include <string.h>                                    // strlen

#include <mongoc/mongoc.h>                             // mongoc_*
#include "kalloc/kaAlloc.h"                            // kaAlloc
#include "kalloc/kaStrdup.h"                           // kaStrdup
#include "ktrace/kTrace.h"                             // KT_E

#include "db/DbDriver.h"                               // DbContextRow, DB_OK, DB_ERR

#include "currentState/mongoc/mongocContext.h"         // Own interface



// -----------------------------------------------------------------------------
//
// Reserved DB + collection names.
//
#define CONTEXT_DB_NAME      "swBroker"
#define CONTEXT_COLLECTION   "contexts"



// -----------------------------------------------------------------------------
//
// Shared from mongocInit.c
//
extern mongoc_client_pool_t* poolP;



// -----------------------------------------------------------------------------
//
// mongocContextSave - upsert by _id.
//
int mongocContextSave(const char* id, const char* url, int kind, const char* body)
{
  if (id == NULL || body == NULL || poolP == NULL)
    return DB_ERR;

  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, CONTEXT_DB_NAME, CONTEXT_COLLECTION);

  bson_t filter;
  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", id);

  bson_t doc;
  bson_init(&doc);
  bson_t setDoc;
  BSON_APPEND_DOCUMENT_BEGIN(&doc, "$set", &setDoc);
  if (url != NULL)
    BSON_APPEND_UTF8(&setDoc, "url", url);
  BSON_APPEND_INT32(&setDoc, "kind", kind);
  BSON_APPEND_UTF8(&setDoc, "body", body);
  bson_append_document_end(&doc, &setDoc);

  bson_t opts;
  bson_init(&opts);
  BSON_APPEND_BOOL(&opts, "upsert", true);

  bson_error_t error;
  bool ok = mongoc_collection_update_one(collP, &filter, &doc, &opts, NULL, &error);

  int result = ok ? DB_OK : DB_ERR;

  if (!ok)
    KT_E("mongoc: contextSave failed for '%s': %s", id, error.message);

  bson_destroy(&opts);
  bson_destroy(&doc);
  bson_destroy(&filter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}



// -----------------------------------------------------------------------------
//
// mongocContextDelete -
//
int mongocContextDelete(const char* id)
{
  if (id == NULL || poolP == NULL)
    return DB_ERR;

  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, CONTEXT_DB_NAME, CONTEXT_COLLECTION);

  bson_t filter;
  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", id);

  bson_error_t error;
  bool ok = mongoc_collection_delete_one(collP, &filter, NULL, NULL, &error);

  int result = ok ? DB_OK : DB_ERR;
  if (!ok)
    KT_E("mongoc: contextDelete failed for '%s': %s", id, error.message);

  bson_destroy(&filter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}



// -----------------------------------------------------------------------------
//
// mongocContextList -
//
int mongocContextList(KAlloc* allocP, DbContextRow** rowsPP, int* countP)
{
  *rowsPP = NULL;
  *countP = 0;

  if (poolP == NULL)
    return DB_ERR;

  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, CONTEXT_DB_NAME, CONTEXT_COLLECTION);

  bson_t emptyFilter;
  bson_init(&emptyFilter);

  mongoc_cursor_t* cursorP = mongoc_collection_find_with_opts(collP, &emptyFilter, NULL, NULL);

  //
  // Two-pass: count, then fill. Avoids realloc dance.
  //
  int            count = 0;
  const bson_t*  doc   = NULL;

  while (mongoc_cursor_next(cursorP, &doc))
    count++;

  bson_error_t cerr;
  if (mongoc_cursor_error(cursorP, &cerr))
  {
    KT_E("mongoc: contextList cursor error: %s", cerr.message);
    mongoc_cursor_destroy(cursorP);
    bson_destroy(&emptyFilter);
    mongoc_collection_destroy(collP);
    mongoc_client_pool_push(poolP, clientP);
    return DB_ERR;
  }

  mongoc_cursor_destroy(cursorP);

  if (count == 0)
  {
    bson_destroy(&emptyFilter);
    mongoc_collection_destroy(collP);
    mongoc_client_pool_push(poolP, clientP);
    return DB_OK;
  }

  DbContextRow* rows = (DbContextRow*) kaAlloc(allocP, count * sizeof(DbContextRow));
  if (rows == NULL)
  {
    bson_destroy(&emptyFilter);
    mongoc_collection_destroy(collP);
    mongoc_client_pool_push(poolP, clientP);
    return DB_ERR;
  }

  cursorP = mongoc_collection_find_with_opts(collP, &emptyFilter, NULL, NULL);
  int ix = 0;

  while (mongoc_cursor_next(cursorP, &doc) && ix < count)
  {
    DbContextRow* r = &rows[ix];
    r->id   = NULL;
    r->url  = NULL;
    r->kind = 0;
    r->body = NULL;

    bson_iter_t it;
    if (bson_iter_init(&it, doc))
    {
      while (bson_iter_next(&it))
      {
        const char* k = bson_iter_key(&it);

        if      ((strcmp(k, "_id")  == 0) && BSON_ITER_HOLDS_UTF8(&it))     r->id   = kaStrdup(allocP, bson_iter_utf8(&it, NULL));
        else if ((strcmp(k, "url")  == 0) && BSON_ITER_HOLDS_UTF8(&it))     r->url  = kaStrdup(allocP, bson_iter_utf8(&it, NULL));
        else if ((strcmp(k, "kind") == 0) && BSON_ITER_HOLDS_INT32(&it))    r->kind = bson_iter_int32(&it);
        else if ((strcmp(k, "body") == 0) && BSON_ITER_HOLDS_UTF8(&it))     r->body = kaStrdup(allocP, bson_iter_utf8(&it, NULL));
      }
    }

    ix++;
  }

  mongoc_cursor_destroy(cursorP);
  bson_destroy(&emptyFilter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  *rowsPP = rows;
  *countP = ix;
  return DB_OK;
}

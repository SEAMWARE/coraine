//
// FILE            mongocEntityBulkCreate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// mongoc bulk insert — single round-trip for the whole batch.
//
// Uses mongoc_collection_insert_many with {ordered: false} so that a
// duplicate-key failure on one document does not short-circuit the rest.
// Per-entity outcomes are recovered from the reply's `writeErrors` array
// (index + code); code 11000 is mongoc's duplicate-key error.
//

#include <string.h>                                   // memset

#include <mongoc/mongoc.h>                            // mongoc_*

#include "ktrace/kTrace.h"                            // KT_E
#include "kjson/KjNode.h"                             // KjNode

#include "db/DbDriver.h"                              // DB_OK, DB_ALREADY_EXISTS, DB_ERR, Tenant

#include "currentState/mongoc/mongocKjTreeToBson.h"   // mongocKjTreeToBson
#include "currentState/mongoc/mongocGeoIndex.h"       // mongocGeoIndexEnsure
#include "currentState/mongoc/mongocEntityBulkCreate.h"  // Own interface



extern mongoc_client_pool_t* poolP;



// -----------------------------------------------------------------------------
//
// countEntries - count children of a KjArray
//
static int countEntries(KjNode* arrP)
{
  int n = 0;
  for (KjNode* c = arrP->value.firstChildP; c != NULL; c = c->next) n++;
  return n;
}



// -----------------------------------------------------------------------------
//
// applyWriteErrors - walk reply.writeErrors, mark the offending entries
// with the right error code.
//
// Each writeError carries { "index": <int>, "code": <int>, "errmsg": ... }.
// Mongo reports code 11000 for duplicate-key violations.
//
static void applyWriteErrors(const bson_t* reply, int* resultsV, int n)
{
  if (reply == NULL) return;

  bson_iter_t top;
  if (!bson_iter_init_find(&top, reply, "writeErrors"))
    return;

  bson_iter_t arr;
  if (!bson_iter_recurse(&top, &arr))
    return;

  while (bson_iter_next(&arr))
  {
    bson_iter_t doc;
    if (!bson_iter_recurse(&arr, &doc))
      continue;

    int idx  = -1;
    int code = 0;
    while (bson_iter_next(&doc))
    {
      const char* key = bson_iter_key(&doc);
      if      (strcmp(key, "index") == 0) idx  = bson_iter_int32(&doc);
      else if (strcmp(key, "code")  == 0) code = bson_iter_int32(&doc);
    }

    if (idx < 0 || idx >= n)
      continue;

    resultsV[idx] = (code == 11000) ? DB_ALREADY_EXISTS : DB_ERR;
  }
}



// -----------------------------------------------------------------------------
//
// mongocEntityBulkCreate -
//
int mongocEntityBulkCreate(Tenant* tenantP, KjNode* entitiesArr, int* resultsV)
{
  if (entitiesArr == NULL || entitiesArr->type != KjArray)
    return DB_ERR;

  int n = countEntries(entitiesArr);
  if (n == 0)
    return DB_ERR;

  //
  // Build one bson_t per entity. Keep the backing storage in a flat
  // array so we can destroy everything in one loop at the end.
  //
  bson_t*  docs    = (bson_t*)  bson_malloc0(sizeof(bson_t)  * n);
  bson_t** docPtrs = (bson_t**) bson_malloc0(sizeof(bson_t*) * n);

  int i = 0;
  for (KjNode* inP = entitiesArr->value.firstChildP; inP != NULL; inP = inP->next, i++)
  {
    mongocKjTreeToBson(inP, &docs[i]);
    docPtrs[i] = &docs[i];
    resultsV[i] = DB_OK;   // optimistic default; overwritten by applyWriteErrors
  }

  //
  // ordered=false so one dup key does NOT abort the remaining inserts.
  //
  bson_t opts = BSON_INITIALIZER;
  BSON_APPEND_BOOL(&opts, "ordered", false);

  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  bson_t       reply;
  bson_error_t error;
  bool ok = mongoc_collection_insert_many(collP,
                                          (const bson_t**) docPtrs,
                                          n,
                                          &opts,
                                          &reply,
                                          &error);

  //
  // Any per-doc failure ends up in reply.writeErrors regardless of the
  // overall ok flag, because ordered=false. Parse that to mark the
  // offending entries; the rest stay DB_OK (their default).
  //
  applyWriteErrors(&reply, resultsV, n);

  if (!ok)
  {
    //
    // A transport-level error (no reply) — nothing to distribute, so
    // flag everything as DB_ERR.
    //
    bool hadWriteErrors = false;
    for (int k = 0; k < n; k++)
    {
      if (resultsV[k] != DB_OK) { hadWriteErrors = true; break; }
    }
    if (!hadWriteErrors)
    {
      KT_E("mongoc: entityBulkCreate transport failure: %s", error.message);
      for (int k = 0; k < n; k++) resultsV[k] = DB_ERR;
    }
  }

  //
  // Geo-index ensure — run per successfully-inserted entity. Skipping
  // the failed ones avoids wasted re-scans but has no correctness
  // impact (mongocGeoIndexEnsure is idempotent).
  //
  {
    int idx = 0;
    for (KjNode* inP = entitiesArr->value.firstChildP; inP != NULL; inP = inP->next, idx++)
    {
      if (resultsV[idx] == DB_OK)
        mongocGeoIndexEnsure(tenantP, inP, collP);
    }
  }

  bson_destroy(&reply);
  bson_destroy(&opts);
  for (int k = 0; k < n; k++)
    bson_destroy(&docs[k]);
  bson_free(docs);
  bson_free(docPtrs);

  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  bool anyOk = false;
  for (int k = 0; k < n; k++) if (resultsV[k] == DB_OK) { anyOk = true; break; }
  return anyOk ? DB_OK : DB_ERR;
}

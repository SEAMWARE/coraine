//
// FILE            mongocEntityBulkUpdate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// mongoc bulk update — two round-trips: one pre-check to learn which
// _ids exist, then one bulk_operation_execute with replace_one for
// the subset that does. Missing ids are flagged DB_NOT_FOUND without
// ever going through the bulk.
//
// Rationale: mongoc_collection_bulk_operation doesn't expose per-op
// status, so we can't distinguish "id not found" from "replace-no-op"
// from the reply. Pre-checking existence costs us one extra round-trip
// but keeps the per-entity BatchOperationResult accurate.
//

#include <string.h>                                      // strcmp, strcpy, memset

#include <mongoc/mongoc.h>                               // mongoc_*

#include "ktrace/kTrace.h"                               // KT_E
#include "kjson/KjNode.h"                                // KjNode
#include "kjson/kjLookup.h"                              // kjLookup

#include "db/DbDriver.h"                                 // DB_OK, DB_NOT_FOUND, DB_ERR, Tenant

#include "currentState/mongoc/mongocKjTreeToBson.h"      // mongocKjTreeToBson
#include "corNgsild/CorNgsild.h"                          // corNgsild (geoConflictAttr)
#include "currentState/mongoc/mongocGeoIndex.h"          // mongocGeoIndexEnsure
#include "currentState/mongoc/mongocEntityBulkUpdate.h"  // Own interface



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
// entityIdAt - return the id string of the ix-th entity, or NULL
//
static const char* entityIdAt(KjNode* entitiesArr, int ix)
{
  int i = 0;
  for (KjNode* e = entitiesArr->value.firstChildP; e != NULL; e = e->next, i++)
  {
    if (i != ix) continue;
    KjNode* idP = kjLookup(e, "id");
    if (idP == NULL || idP->type != KjString) return NULL;
    return idP->value.s;
  }
  return NULL;
}



// -----------------------------------------------------------------------------
//
// mongocEntityBulkUpdate -
//
int mongocEntityBulkUpdate(Tenant* tenantP, KjNode* entitiesArr, int* resultsV)
{
  if (entitiesArr == NULL || entitiesArr->type != KjArray)
    return DB_ERR;

  int n = countEntries(entitiesArr);
  if (n == 0)
    return DB_ERR;

  //
  // Pass 1 — pre-check existence with find({_id: {$in: [ids]}}). Build
  // the list of ids and a parallel "exists" bool array.
  //
  bool* existsV = (bool*) bson_malloc0(sizeof(bool) * n);

  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  {
    bson_t filter = BSON_INITIALIZER;
    bson_t inDoc;
    BSON_APPEND_DOCUMENT_BEGIN(&filter, "_id", &inDoc);

    bson_t idArr;
    BSON_APPEND_ARRAY_BEGIN(&inDoc, "$in", &idArr);

    for (int i = 0; i < n; i++)
    {
      const char* id = entityIdAt(entitiesArr, i);
      if (id == NULL) continue;
      char key[16];
      snprintf(key, sizeof(key), "%d", i);
      BSON_APPEND_UTF8(&idArr, key, id);
    }

    bson_append_array_end(&inDoc, &idArr);
    bson_append_document_end(&filter, &inDoc);

    bson_t projection = BSON_INITIALIZER;
    BSON_APPEND_INT32(&projection, "_id", 1);

    bson_t opts = BSON_INITIALIZER;
    BSON_APPEND_DOCUMENT(&opts, "projection", &projection);

    mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(collP, &filter, &opts, NULL);

    const bson_t* docP = NULL;
    while (mongoc_cursor_next(cursor, &docP))
    {
      bson_iter_t iter;
      if (!bson_iter_init_find(&iter, docP, "_id") || !BSON_ITER_HOLDS_UTF8(&iter))
        continue;

      const char* foundId = bson_iter_utf8(&iter, NULL);

      for (int i = 0; i < n; i++)
      {
        const char* id = entityIdAt(entitiesArr, i);
        if (id != NULL && strcmp(id, foundId) == 0)
        {
          existsV[i] = true;
          break;
        }
      }
    }

    mongoc_cursor_destroy(cursor);
    bson_destroy(&opts);
    bson_destroy(&projection);
    bson_destroy(&filter);
  }

  //
  // Pass 2 — bulk replace_one for the subset that exists. Missing ids
  // → DB_NOT_FOUND, not touched in the bulk.
  //
  mongoc_bulk_operation_t* bulk = NULL;
  int bulkCount = 0;

  for (int i = 0; i < n; i++)
  {
    if (!existsV[i])
    {
      resultsV[i] = DB_NOT_FOUND;
      continue;
    }

    int ix = 0;
    KjNode* entityP = NULL;
    for (KjNode* e = entitiesArr->value.firstChildP; e != NULL; e = e->next, ix++)
    {
      if (ix == i) { entityP = e; break; }
    }

    //
    // Ensure this entity's 2dsphere indexes BEFORE staging it. A name it declares
    // a GeoProperty while the tenant already holds it as another type cannot be
    // stored, so it is refused and left out of the batch, its siblings unaffected.
    // Already-indexed attributes cost one string compare.
    //
    const char* geoClashP = mongocGeoIndexEnsure(tenantP, entityP, collP);
    if (geoClashP != NULL)
    {
      KT_E("mongoc: entityBulkUpdate: '%s' is a GeoProperty here but already held as another type", geoClashP);
      corNgsild.geoConflictAttr = geoClashP;
      resultsV[i] = DB_GEO_TYPE_CONFLICT;
      continue;
    }

    if (bulk == NULL)
    {
      bson_t bulkOpts = BSON_INITIALIZER;
      BSON_APPEND_BOOL(&bulkOpts, "ordered", false);
      bulk = mongoc_collection_create_bulk_operation_with_opts(collP, &bulkOpts);
      bson_destroy(&bulkOpts);
    }

    const char* id = entityIdAt(entitiesArr, i);
    bson_t selector = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&selector, "_id", id);

    bson_t doc;
    mongocKjTreeToBson(entityP, &doc);

    mongoc_bulk_operation_replace_one(bulk, &selector, &doc, false);

    bson_destroy(&selector);
    bson_destroy(&doc);

    resultsV[i] = DB_OK;  // optimistic; if bulk reports an error we downgrade below
    bulkCount++;
  }

  if (bulk != NULL)
  {
    bson_t       reply;
    bson_error_t error;
    bool ok = mongoc_bulk_operation_execute(bulk, &reply, &error) > 0;
    if (!ok)
    {
      KT_E("mongoc: entityBulkUpdate execute failed: %s", error.message);

      //
      // The bulk reply does not say WHICH entry failed, but for the one failure
      // that has a better answer than 500 it does not have to: an entity of this
      // batch holding a geo-indexed name as another type is a clash of Attribute
      // kinds, and that is decided from our own payload plus the geo-index cache.
      // Everything else stays DB_ERR, and only a failing batch pays for the walk.
      //
      bool geoClash = (strstr(error.message, "Can't extract geo keys") != NULL);
      int  i        = 0;

      for (KjNode* inP = entitiesArr->value.firstChildP; inP != NULL; inP = inP->next, i++)
      {
        if (resultsV[i] != DB_OK)
          continue;

        const char* mixedP = geoClash ? mongocGeoIndexMixedName(tenantP, inP) : NULL;

        if (mixedP != NULL)
        {
          KT_E("mongoc: entityBulkUpdate: '%s' is held as a GeoProperty here and updated to another type", mixedP);
          corNgsild.geoConflictAttr = mixedP;
          resultsV[i] = DB_GEO_TYPE_CONFLICT;
        }
        else
          resultsV[i] = DB_ERR;
      }
    }
    bson_destroy(&reply);
    mongoc_bulk_operation_destroy(bulk);
  }

  bson_free(existsV);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  bool anyOk = false;
  for (int k = 0; k < n; k++) if (resultsV[k] == DB_OK) { anyOk = true; break; }
  return anyOk ? DB_OK : DB_ERR;
}

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
#include "swNgsild/SwNgsild.h"                          // swNgsild (geoConflictAttr)
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
static KjNode* entityAt(KjNode* entitiesArr, int ix)
{
  int i = 0;
  for (KjNode* inP = entitiesArr->value.firstChildP; inP != NULL; inP = inP->next, i++)
  {
    if (i == ix)
      return inP;
  }
  return NULL;
}



static void applyWriteErrors(const bson_t* reply, int* resultsV, int n, const int* batchIx, KjNode* entitiesArr, Tenant* tenantP)
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

    int         idx    = -1;
    int         code   = 0;
    const char* errmsg = NULL;
    while (bson_iter_next(&doc))
    {
      const char* key = bson_iter_key(&doc);
      if      (strcmp(key, "index")  == 0) idx    = bson_iter_int32(&doc);
      else if (strcmp(key, "code")   == 0) code   = bson_iter_int32(&doc);
      else if (strcmp(key, "errmsg") == 0 && BSON_ITER_HOLDS_UTF8(&doc)) errmsg = bson_iter_utf8(&doc, NULL);
    }

    if (idx < 0 || idx >= n)
      continue;

    int entityIx = (batchIx != NULL) ? batchIx[idx] : idx;

    if (code == 11000)
    {
      resultsV[entityIx] = DB_ALREADY_EXISTS;
      continue;
    }

    //
    // The same two causes as the single-entity paths, distinguished only here on
    // the failing entity: a name geo-indexed in this tenant but created with
    // another type is a clash of Attribute kinds (→ 409), otherwise S2 refused
    // the geometry itself (→ 400).
    //
    if (errmsg != NULL && strstr(errmsg, "Can't extract geo keys") != NULL)
    {
      const char* mixedP = mongocGeoIndexMixedName(tenantP, entityAt(entitiesArr, entityIx));
      if (mixedP != NULL)
      {
        KT_E("mongoc: entityBulkCreate: '%s' is held as a GeoProperty here and created with another type", mixedP);
        swNgsild.geoConflictAttr = mixedP;
        resultsV[entityIx] = DB_GEO_TYPE_CONFLICT;
      }
      else
      {
        KT_E("mongoc: entityBulkCreate rejected by 2dsphere: %s", errmsg);
        resultsV[entityIx] = DB_INVALID_GEOMETRY;
      }
      continue;
    }

    resultsV[entityIx] = DB_ERR;
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
  int*     batchIx = (int*)     bson_malloc0(sizeof(int)     * n);   // batch slot -> entity index

  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  //
  // Ensure each entity's 2dsphere indexes BEFORE the batch. An entity naming an
  // Attribute as a GeoProperty where the tenant already holds it as another type
  // cannot be stored at all, so it is refused here and left OUT of the batch —
  // its siblings are unaffected, which is what a batch is for. Every already
  // indexed attribute costs one string compare, so the ordinary batch is
  // unaffected too.
  //
  int batchN = 0;
  int i      = 0;
  for (KjNode* inP = entitiesArr->value.firstChildP; inP != NULL; inP = inP->next, i++)
  {
    const char* geoClashP = mongocGeoIndexEnsure(tenantP, inP, collP);

    if (geoClashP != NULL)
    {
      KT_E("mongoc: entityBulkCreate: '%s' is a GeoProperty here but already held as another type", geoClashP);
      swNgsild.geoConflictAttr = geoClashP;
      resultsV[i] = DB_GEO_TYPE_CONFLICT;
      continue;
    }

    resultsV[i] = DB_OK;   // optimistic default; overwritten by applyWriteErrors
    mongocKjTreeToBson(inP, &docs[batchN]);
    docPtrs[batchN] = &docs[batchN];
    batchIx[batchN] = i;
    batchN++;
  }

  //
  // ordered=false so one dup key does NOT abort the remaining inserts.
  //
  bson_t opts = BSON_INITIALIZER;
  BSON_APPEND_BOOL(&opts, "ordered", false);

  bson_t       reply;
  bson_error_t error;
  bool         ok = true;

  if (batchN > 0)
    ok = mongoc_collection_insert_many(collP,
                                       (const bson_t**) docPtrs,
                                       batchN,
                                       &opts,
                                       &reply,
                                       &error);
  else
    bson_init(&reply);

  //
  // Any per-doc failure ends up in reply.writeErrors regardless of the
  // overall ok flag, because ordered=false. Parse that to mark the
  // offending entries; the rest stay DB_OK (their default). Write-error
  // indices are BATCH slots, so they are mapped back through batchIx.
  //
  applyWriteErrors(&reply, resultsV, batchN, batchIx, entitiesArr, tenantP);

  if (!ok)
  {
    //
    // A transport-level error (no reply) — nothing to distribute, so flag every
    // entity that actually went INTO the batch as DB_ERR. Entities held back for a
    // geo type conflict never reached mongo and keep their own verdict; they are
    // also why "did any batched entity fail?" is asked over batch slots and not
    // over resultsV, which already carries those conflicts.
    //
    bool hadWriteErrors = false;
    for (int k = 0; k < batchN; k++)
    {
      if (resultsV[batchIx[k]] != DB_OK) { hadWriteErrors = true; break; }
    }
    if (!hadWriteErrors)
    {
      KT_E("mongoc: entityBulkCreate transport failure: %s", error.message);
      for (int k = 0; k < batchN; k++) resultsV[batchIx[k]] = DB_ERR;
    }
  }

  bson_destroy(&reply);
  bson_destroy(&opts);
  for (int k = 0; k < batchN; k++)     // only the batched slots were ever initialized
    bson_destroy(&docs[k]);
  bson_free(docs);
  bson_free(docPtrs);
  bson_free(batchIx);

  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  bool anyOk = false;
  for (int k = 0; k < n; k++) if (resultsV[k] == DB_OK) { anyOk = true; break; }
  return anyOk ? DB_OK : DB_ERR;
}

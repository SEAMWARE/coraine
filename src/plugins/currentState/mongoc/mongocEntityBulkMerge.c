//
// FILE            mongocEntityBulkMerge.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// mongoc Batch Merge persistence — two round-trips for the whole batch, with
// the merge itself done by the broker in between:
//
//   mongocEntityBulkRetrieve     one find({_id:{$in:[...]}}) fetches every
//                                current document the batch touches into the
//                                request arena (Phase 1). The broker then runs
//                                ldEntityMerge over each fetched tree.
//   mongocEntityBulkChangesApply for each already-merged target, build a
//                                surgical $set/$unset from its report and stage
//                                an update_one on a single bulk operation
//                                (Phase 2), then one bulk execute (Phase 3).
//
// Fragments whose id was absent from Phase 1 get a NULL target slot; the broker
// flags them DB_NOT_FOUND and they never reach Phase 2. Fragments whose merge
// produced no writes are skipped server-side but still count as DB_OK so
// notifications and the 207 body reflect reality.
//
// On bulk_execute failure we can't tell per-slot which update failed, so every
// staged slot gets demoted to DB_ERR. The underlying driver error is traced.
//

#include <string.h>                                      // strcmp

#include <mongoc/mongoc.h>                               // mongoc_client_*, mongoc_collection_*

#include "corLog/corLog.h"                               // COR_E
#include "corTree/CorNode.h"                             // CorNode
#include "corTree/corTreeLookup.h"                       // corTreeLookup
#include "corRest/CorRestState.h"                          // corRest

#include "corNgsild/ldEntityMerge.h"                      // LdMergeReport

#include "db/DbDriver.h"                                 // DB_OK, DB_NOT_FOUND, DB_ERR, Tenant
#include "currentState/mongoc/mongocBsonToTree.h"        // mongocBsonToTree
#include "currentState/mongoc/mongocEntityMerge.h"       // mongocBuildSurgicalUpdate
#include "corNgsild/CorNgsild.h"                         // corNgsild (geoConflictAttr)
#include "currentState/mongoc/mongocGeoIndex.h"          // mongocGeoIndexEnsure
#include "currentState/mongoc/mongocEntityBulkMerge.h"   // Own interface



// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// countEntries -
//
static int countEntries(CorNode* arrP)
{
  int n = 0;
  for (CorNode* c = arrP->value.head; c != NULL; c = c->next) n++;
  return n;
}



// -----------------------------------------------------------------------------
//
// fragmentAt - the ix-th child of a CorArray
//
static CorNode* fragmentAt(CorNode* arrP, int ix)
{
  int i = 0;
  for (CorNode* c = arrP->value.head; c != NULL; c = c->next, i++)
    if (i == ix) return c;
  return NULL;
}



// -----------------------------------------------------------------------------
//
// mongocEntityBulkRetrieve - Phase 1: fetch every current doc in one $in query.
//
// targetsV is a caller-allocated, zero-initialised array parallel to the
// fragments in `fragmentsArr`. Each slot receives the request-arena tree of the
// fetched document, or stays NULL when the id was not found. Fragments that
// share an id share ONE target tree so the broker's sequential merges see each
// other's results (§ 5.6.10 array-order semantics).
//
int mongocEntityBulkRetrieve(Tenant* tenantP, CorNode* fragmentsArr, CorNode** targetsV)
{
  if (fragmentsArr == NULL || fragmentsArr->type != CorArray)
    return DB_ERR;

  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  bson_t filter = BSON_INITIALIZER;
  bson_t inDoc;
  BSON_APPEND_DOCUMENT_BEGIN(&filter, "_id", &inDoc);

  bson_t idArr;
  BSON_APPEND_ARRAY_BEGIN(&inDoc, "$in", &idArr);

  int ix = 0;
  for (CorNode* fragP = fragmentsArr->value.head; fragP != NULL; fragP = fragP->next, ix++)
  {
    CorNode* idP = corTreeLookup(fragP, "id");
    if (idP == NULL || idP->type != CorString) continue;
    char key[16];
    snprintf(key, sizeof(key), "%d", ix);
    BSON_APPEND_UTF8(&idArr, key, idP->value.s);
  }

  bson_append_array_end(&inDoc, &idArr);
  bson_append_document_end(&filter, &inDoc);

  mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(collP, &filter, NULL, NULL);

  const bson_t* doc = NULL;
  while (mongoc_cursor_next(cursor, &doc))
  {
    bson_iter_t iter;
    if (!bson_iter_init_find(&iter, doc, "_id") || !BSON_ITER_HOLDS_UTF8(&iter))
      continue;

    const char* foundId = bson_iter_utf8(&iter, NULL);

    // One fetched doc may back several fragment slots (multiple fragments for
    // the same entity id in the batch). The shared target lets the broker's
    // sequential merges accumulate, and ordered=true on the bulk preserves that
    // order server-side.
    CorNode* shared = NULL;
    int k = 0;
    for (CorNode* fragP = fragmentsArr->value.head; fragP != NULL; fragP = fragP->next, k++)
    {
      if (targetsV[k] != NULL) continue;
      CorNode* idP = corTreeLookup(fragP, "id");
      if (idP == NULL || idP->type != CorString) continue;
      if (strcmp(idP->value.s, foundId) != 0) continue;
      if (shared == NULL)
        shared = mongocBsonToTree(&corRest.kalloc, doc);
      targetsV[k] = shared;
    }
  }

  bson_error_t cursorError;
  if (mongoc_cursor_error(cursor, &cursorError))
    COR_E("mongoc: entityBulkRetrieve $in fetch failed: %s", cursorError.message);

  mongoc_cursor_destroy(cursor);
  bson_destroy(&filter);

  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return DB_OK;
}



// -----------------------------------------------------------------------------
//
// writeErrorsApply - an unordered bulk: mark the entities the reply's writeErrors names
//
// Each writeError is { "index": <bulk op>, "errmsg": ... }; the index is a BULK slot, mapped back to the
// entity through batchIx. Returns how many were marked: none means a reply that named no failure (a
// transport error), where nothing can be told apart.
//
static int writeErrorsApply(const bson_t* reply, int* resultsV, int batchN, const int* batchIx, CorNode** mergedTargetsV, Tenant* tenantP)
{
  bson_iter_t top;
  bson_iter_t arr;

  if (!bson_iter_init_find(&top, reply, "writeErrors") || !bson_iter_recurse(&top, &arr))
    return 0;

  int marked = 0;

  while (bson_iter_next(&arr))
  {
    bson_iter_t doc;

    if (!bson_iter_recurse(&arr, &doc))
      continue;

    int         idx    = -1;
    const char* errmsg = NULL;

    while (bson_iter_next(&doc))
    {
      const char* key = bson_iter_key(&doc);

      if      (strcmp(key, "index")  == 0) idx    = bson_iter_int32(&doc);
      else if (strcmp(key, "errmsg") == 0 && BSON_ITER_HOLDS_UTF8(&doc)) errmsg = bson_iter_utf8(&doc, NULL);
    }

    if ((idx < 0) || (idx >= batchN))
      continue;

    int         entityIx = batchIx[idx];
    const char* mixedP   = ((errmsg != NULL) && (strstr(errmsg, "Can't extract geo keys") != NULL)) ? mongocGeoIndexMixedName(tenantP, mergedTargetsV[entityIx]) : NULL;

    ++marked;

    if (mixedP != NULL)
    {
      COR_E("mongoc: entityBulkChangesApply: '%s' is held as a GeoProperty here and merged as another type", mixedP);
      corNgsild.geoConflictAttr = mixedP;
      resultsV[entityIx]        = DB_GEO_TYPE_CONFLICT;
    }
    else
      resultsV[entityIx] = DB_ERR;
  }

  return marked;
}



// -----------------------------------------------------------------------------
//
// mongocEntityBulkChangesApply - Phase 2/3: stage + execute the surgical bulk.
//
// `mergedTargetsV` are the already-merged trees (broker ran the merge engine);
// `reportsV[i]` describes fragment i's changes; `resultsV[i]` is DB_OK for slots
// the broker merged (others are skipped here). On bulk execute failure every
// staged slot is demoted to DB_ERR.
//
int mongocEntityBulkChangesApply(Tenant* tenantP, CorNode* fragmentsArr,
                                 CorNode** mergedTargetsV, LdMergeReport* reportsV,
                                 int* resultsV)
{
  if (fragmentsArr == NULL || fragmentsArr->type != CorArray)
    return DB_ERR;

  int n = countEntries(fragmentsArr);
  if (n == 0)
    return DB_ERR;

  bool* staged  = (bool*) bson_malloc0(sizeof(bool) * n);
  int*  batchIx = (int*)  bson_malloc0(sizeof(int) * n);   // bulk slot -> entity index
  int   batchN  = 0;

  //
  // Ordered only when an id comes more than once (a batch merge with the same entity twice): its
  // updates apply in array order, and the first that fails stops the rest. Every id distinct - a batch
  // update or upsert, most merges - the bulk is unordered: one entity refused (a geo-type clash only
  // the write sees) leaves the others written, and the reply's writeErrors names it.
  //
  bool ordered = false;

  for (int i = 0; (i < n) && (ordered == false); i++)
  {
    CorNode* aP = corTreeLookup(fragmentAt(fragmentsArr, i), "id");

    for (int j = i + 1; (aP != NULL) && (aP->type == CorString) && (j < n); j++)
    {
      CorNode* bP = corTreeLookup(fragmentAt(fragmentsArr, j), "id");

      if ((bP != NULL) && (bP->type == CorString) && (strcmp(aP->value.s, bP->value.s) == 0))
      {
        ordered = true;
        break;
      }
    }
  }

  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  mongoc_bulk_operation_t* bulk = NULL;

  for (int i = 0; i < n; i++)
  {
    if (resultsV[i] != DB_OK)
      continue;

    CorNode* fragP = fragmentAt(fragmentsArr, i);
    CorNode* idP  = (fragP != NULL) ? corTreeLookup(fragP, "id") : NULL;
    if (idP == NULL || idP->type != CorString || mergedTargetsV[i] == NULL)
      continue;

    bson_t update;
    bson_init(&update);
    bool noChanges = true;
    mongocBuildSurgicalUpdate(mergedTargetsV[i], &reportsV[i], &update, &noChanges);

    if (noChanges)
    {
      bson_destroy(&update);
      continue;
    }

    //
    // Ensure the merged entity's 2dsphere indexes BEFORE staging it. A name the
    // merge turns into a GeoProperty while the tenant already holds it as another
    // type cannot be stored, so it is refused and left out of the batch, its
    // siblings unaffected. Already-indexed attributes cost one string compare.
    //
    const char* geoClashP = mongocGeoIndexEnsure(tenantP, mergedTargetsV[i], collP);
    if (geoClashP != NULL)
    {
      COR_E("mongoc: entityBulkChangesApply: '%s' is a GeoProperty here but already held as another type", geoClashP);
      corNgsild.geoConflictAttr = geoClashP;
      resultsV[i] = DB_GEO_TYPE_CONFLICT;
      bson_destroy(&update);
      continue;
    }

    if (bulk == NULL)
    {
      bson_t bulkOpts = BSON_INITIALIZER;

      BSON_APPEND_BOOL(&bulkOpts, "ordered", ordered);
      bulk = mongoc_collection_create_bulk_operation_with_opts(collP, &bulkOpts);
      bson_destroy(&bulkOpts);
    }

    bson_t selector = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&selector, "_id", idP->value.s);

    bson_error_t stageErr;
    if (!mongoc_bulk_operation_update_one_with_opts(bulk, &selector, &update, NULL, &stageErr))
    {
      COR_E("mongoc: entityBulkChangesApply stage failed for %s: %s", idP->value.s, stageErr.message);
      resultsV[i] = DB_ERR;
    }
    else
    {
      staged[i]         = true;
      batchIx[batchN++] = i;
    }

    bson_destroy(&selector);
    bson_destroy(&update);
  }

  if (bulk != NULL)
  {
    bson_t       reply;
    bson_error_t error;
    bool ok = mongoc_bulk_operation_execute(bulk, &reply, &error) > 0;
    if (!ok)
    {
      COR_E("mongoc: entityBulkChangesApply execute failed: %s", error.message);

      //
      // Same as the single-entity merge, decided per staged fragment from the
      // merged tree plus the geo-index cache: a name geo-indexed in this tenant
      // and merged as another type is a clash of Attribute kinds, not a 500.
      //
      bool geoClash = (strstr(error.message, "Can't extract geo keys") != NULL);

      //
      // Unordered: every update that did not fail was written - only the ones the reply names are not
      //
      bool decided = (ordered == false) && (writeErrorsApply(&reply, resultsV, batchN, batchIx, mergedTargetsV, tenantP) > 0);

      for (int i = 0; (decided == false) && (i < n); i++)
      {
        if (!staged[i])
          continue;

        const char* mixedP = geoClash ? mongocGeoIndexMixedName(tenantP, mergedTargetsV[i]) : NULL;

        if (mixedP != NULL)
        {
          COR_E("mongoc: entityBulkChangesApply: '%s' is held as a GeoProperty here and merged as another type", mixedP);
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

  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  bson_free(staged);
  bson_free(batchIx);

  bool anyOk = false;
  for (int k = 0; k < n; k++) if (resultsV[k] == DB_OK) { anyOk = true; break; }
  return anyOk ? DB_OK : DB_ERR;
}

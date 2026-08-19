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

#include "ktrace/kTrace.h"                               // KT_E
#include "kjson/KjNode.h"                                // KjNode
#include "kjson/kjLookup.h"                              // kjLookup
#include "corRest/CorRestState.h"                          // corRest

#include "corNgsild/ldEntityMerge.h"                      // LdMergeReport

#include "db/DbDriver.h"                                 // DB_OK, DB_NOT_FOUND, DB_ERR, Tenant
#include "currentState/mongoc/mongocBsonToKjTree.h"      // mongocBsonToKjTree
#include "currentState/mongoc/mongocEntityMerge.h"       // mongocBuildSurgicalUpdate
#include "corNgsild/CorNgsild.h"                          // corNgsild (geoConflictAttr)
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
static int countEntries(KjNode* arrP)
{
  int n = 0;
  for (KjNode* c = arrP->value.firstChildP; c != NULL; c = c->next) n++;
  return n;
}



// -----------------------------------------------------------------------------
//
// fragmentAt - the ix-th child of a KjArray
//
static KjNode* fragmentAt(KjNode* arrP, int ix)
{
  int i = 0;
  for (KjNode* c = arrP->value.firstChildP; c != NULL; c = c->next, i++)
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
int mongocEntityBulkRetrieve(Tenant* tenantP, KjNode* fragmentsArr, KjNode** targetsV)
{
  if (fragmentsArr == NULL || fragmentsArr->type != KjArray)
    return DB_ERR;

  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  bson_t filter = BSON_INITIALIZER;
  bson_t inDoc;
  BSON_APPEND_DOCUMENT_BEGIN(&filter, "_id", &inDoc);

  bson_t idArr;
  BSON_APPEND_ARRAY_BEGIN(&inDoc, "$in", &idArr);

  int ix = 0;
  for (KjNode* fragP = fragmentsArr->value.firstChildP; fragP != NULL; fragP = fragP->next, ix++)
  {
    KjNode* idP = kjLookup(fragP, "id");
    if (idP == NULL || idP->type != KjString) continue;
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
    KjNode* shared = NULL;
    int k = 0;
    for (KjNode* fragP = fragmentsArr->value.firstChildP; fragP != NULL; fragP = fragP->next, k++)
    {
      if (targetsV[k] != NULL) continue;
      KjNode* idP = kjLookup(fragP, "id");
      if (idP == NULL || idP->type != KjString) continue;
      if (strcmp(idP->value.s, foundId) != 0) continue;
      if (shared == NULL)
        shared = mongocBsonToKjTree(&corRest.kalloc, doc);
      targetsV[k] = shared;
    }
  }

  bson_error_t cursorError;
  if (mongoc_cursor_error(cursor, &cursorError))
    KT_E("mongoc: entityBulkRetrieve $in fetch failed: %s", cursorError.message);

  mongoc_cursor_destroy(cursor);
  bson_destroy(&filter);

  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return DB_OK;
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
int mongocEntityBulkChangesApply(Tenant* tenantP, KjNode* fragmentsArr,
                                 KjNode** mergedTargetsV, LdMergeReport* reportsV,
                                 int* resultsV)
{
  if (fragmentsArr == NULL || fragmentsArr->type != KjArray)
    return DB_ERR;

  int n = countEntries(fragmentsArr);
  if (n == 0)
    return DB_ERR;

  bool* staged = (bool*) bson_malloc0(sizeof(bool) * n);

  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  mongoc_bulk_operation_t* bulk = NULL;

  for (int i = 0; i < n; i++)
  {
    if (resultsV[i] != DB_OK)
      continue;

    KjNode* fragP = fragmentAt(fragmentsArr, i);
    KjNode* idP   = (fragP != NULL) ? kjLookup(fragP, "id") : NULL;
    if (idP == NULL || idP->type != KjString || mergedTargetsV[i] == NULL)
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
      KT_E("mongoc: entityBulkChangesApply: '%s' is a GeoProperty here but already held as another type", geoClashP);
      corNgsild.geoConflictAttr = geoClashP;
      resultsV[i] = DB_GEO_TYPE_CONFLICT;
      bson_destroy(&update);
      continue;
    }

    if (bulk == NULL)
    {
      // ordered=true so multi-instance same-id fragments apply in array order.
      bulk = mongoc_collection_create_bulk_operation_with_opts(collP, NULL);
    }

    bson_t selector = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&selector, "_id", idP->value.s);

    bson_error_t stageErr;
    if (!mongoc_bulk_operation_update_one_with_opts(bulk, &selector, &update, NULL, &stageErr))
    {
      KT_E("mongoc: entityBulkChangesApply stage failed for %s: %s", idP->value.s, stageErr.message);
      resultsV[i] = DB_ERR;
    }
    else
      staged[i] = true;

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
      KT_E("mongoc: entityBulkChangesApply execute failed: %s", error.message);

      //
      // Same as the single-entity merge, decided per staged fragment from the
      // merged tree plus the geo-index cache: a name geo-indexed in this tenant
      // and merged as another type is a clash of Attribute kinds, not a 500.
      //
      bool geoClash = (strstr(error.message, "Can't extract geo keys") != NULL);

      for (int i = 0; i < n; i++)
      {
        if (!staged[i])
          continue;

        const char* mixedP = geoClash ? mongocGeoIndexMixedName(tenantP, mergedTargetsV[i]) : NULL;

        if (mixedP != NULL)
        {
          KT_E("mongoc: entityBulkChangesApply: '%s' is held as a GeoProperty here and merged as another type", mixedP);
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

  bool anyOk = false;
  for (int k = 0; k < n; k++) if (resultsV[k] == DB_OK) { anyOk = true; break; }
  return anyOk ? DB_OK : DB_ERR;
}

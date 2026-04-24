//
// FILE            mongocEntityBulkMerge.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// mongoc Batch Merge: two round-trips for the whole batch.
//
//   Phase 1  one find({_id:{$in:[...]}}) fetches every current document
//            the batch touches.
//   Phase 2  for each fragment, look up its pre-fetched target, compute
//            the merge in memory via mongocEntityMergeBuildUpdate, and
//            stage an update_one_with_opts on a single bulk_operation.
//   Phase 3  one bulk_operation_execute runs all surgical updates server
//            side.
//
// Fragments whose id was absent from Phase 1 are flagged DB_NOT_FOUND and
// never reach the bulk. Fragments whose merge produced no writes (e.g.
// every field already at the same value) are also skipped server-side but
// still count as DB_OK so notifications and the 207 body reflect reality.
//
// On bulk_execute failure we can't tell per-slot which update failed, so
// every staged slot gets demoted to DB_ERR. The underlying driver error
// is traced.
//

#include <string.h>                                      // strcmp

#include <mongoc/mongoc.h>                               // mongoc_client_*, mongoc_collection_*

#include "ktrace/kTrace.h"                               // KT_E
#include "kjson/KjNode.h"                                // KjNode
#include "kjson/kjLookup.h"                              // kjLookup
#include "swRest/SwRestState.h"                          // swRest

#include "swNgsild/ldEntityMerge.h"                      // LdMergeReport

#include "db/DbDriver.h"                                 // DB_OK, DB_NOT_FOUND, DB_ERR, Tenant
#include "currentState/mongoc/mongocBsonToKjTree.h"      // mongocBsonToKjTree
#include "currentState/mongoc/mongocEntityMerge.h"       // mongocEntityMergeBuildUpdate
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
// mongocEntityBulkMerge -
//
int mongocEntityBulkMerge(Tenant* tenantP, KjNode* fragmentsArr,
                          uint64_t ts, int* resultsV,
                          LdMergeReport* reportsV,
                          KjNode** snapshotsV)
{
  if (fragmentsArr == NULL || fragmentsArr->type != KjArray)
    return DB_ERR;

  int n = countEntries(fragmentsArr);
  if (n == 0)
    return DB_ERR;

  KjNode** targetsV = (KjNode**) bson_malloc0(sizeof(KjNode*) * n);
  bool*    staged   = (bool*)    bson_malloc0(sizeof(bool)    * n);

  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  //
  // Phase 1 — fetch every current doc in a single $in query. Unknown-ids
  // simply won't appear in the result set; the corresponding targetsV slot
  // stays NULL and becomes DB_NOT_FOUND in Phase 2.
  //
  {
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

      // A single fetched doc may back several fragment slots when the batch
      // contains multiple fragments for the same entity id (§ 5.6.10 allows
      // this; the spec says they apply in array order). The shared target
      // pointer lets each subsequent ldEntityMerge see the previous merge's
      // in-memory result, and ordered=true on the bulk op preserves that
      // ordering server-side.
      KjNode* shared = NULL;
      int k = 0;
      for (KjNode* fragP = fragmentsArr->value.firstChildP; fragP != NULL; fragP = fragP->next, k++)
      {
        if (targetsV[k] != NULL) continue;
        KjNode* idP = kjLookup(fragP, "id");
        if (idP == NULL || idP->type != KjString) continue;
        if (strcmp(idP->value.s, foundId) != 0) continue;
        if (shared == NULL)
          shared = mongocBsonToKjTree(&swRest.kalloc, doc);
        targetsV[k] = shared;
      }
    }

    bson_error_t cursorError;
    if (mongoc_cursor_error(cursor, &cursorError))
      KT_E("mongoc: entityBulkMerge $in fetch failed: %s", cursorError.message);

    mongoc_cursor_destroy(cursor);
    bson_destroy(&filter);
  }

  //
  // Phase 2 — merge and stage bulk updates.
  //
  mongoc_bulk_operation_t* bulk = NULL;

  for (int i = 0; i < n; i++)
  {
    KjNode* fragP = fragmentAt(fragmentsArr, i);
    KjNode* idP   = (fragP != NULL) ? kjLookup(fragP, "id") : NULL;

    if (fragP == NULL || idP == NULL || idP->type != KjString)
    {
      resultsV[i] = DB_ERR;
      continue;
    }

    if (targetsV[i] == NULL)
    {
      resultsV[i] = DB_NOT_FOUND;
      continue;
    }

    bson_t update;
    bool   noChanges = true;
    int    rc = mongocEntityMergeBuildUpdate(targetsV[i], fragP, ts, &reportsV[i],
                                              &update, &noChanges);
    if (rc != DB_OK)
    {
      resultsV[i]   = rc;
      snapshotsV[i] = NULL;
      continue;
    }

    snapshotsV[i] = targetsV[i];
    resultsV[i]   = DB_OK;

    if (noChanges)
    {
      bson_destroy(&update);
      continue;
    }

    if (bulk == NULL)
    {
      // ordered=true so multi-instance same-id fragments apply in array
      // order on the server (matches the in-memory merge order).
      bulk = mongoc_collection_create_bulk_operation_with_opts(collP, NULL);
    }

    bson_t selector = BSON_INITIALIZER;
    BSON_APPEND_UTF8(&selector, "_id", idP->value.s);

    bson_error_t stageErr;
    if (!mongoc_bulk_operation_update_one_with_opts(bulk, &selector, &update, NULL, &stageErr))
    {
      KT_E("mongoc: entityBulkMerge stage failed for %s: %s", idP->value.s, stageErr.message);
      resultsV[i]   = DB_ERR;
      snapshotsV[i] = NULL;
    }
    else
    {
      staged[i] = true;
    }

    bson_destroy(&selector);
    bson_destroy(&update);
  }

  //
  // Phase 3 — one execute for the whole batch.
  //
  if (bulk != NULL)
  {
    bson_t       reply;
    bson_error_t error;
    bool ok = mongoc_bulk_operation_execute(bulk, &reply, &error) > 0;
    if (!ok)
    {
      KT_E("mongoc: entityBulkMerge execute failed: %s", error.message);
      // No per-op status — downgrade every staged slot.
      for (int i = 0; i < n; i++)
      {
        if (staged[i])
        {
          resultsV[i]   = DB_ERR;
          snapshotsV[i] = NULL;
        }
      }
    }
    bson_destroy(&reply);
    mongoc_bulk_operation_destroy(bulk);
  }

  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  bson_free(targetsV);
  bson_free(staged);

  bool anyOk = false;
  for (int k = 0; k < n; k++) if (resultsV[k] == DB_OK) { anyOk = true; break; }
  return anyOk ? DB_OK : DB_ERR;
}

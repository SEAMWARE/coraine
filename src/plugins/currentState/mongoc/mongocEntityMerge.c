//
// FILE            mongocEntityMerge.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// mongoc entityMerge: fetch the current document, apply ldEntityMerge in
// memory, then write only the attributes that actually changed using a
// surgical $set / $unset update. A PATCH that modifies one attribute on a
// 2000-attribute entity transfers only that attribute plus the entity-level
// modifiedAt, not the whole document.
//
// Factored so that mongocEntityMergeOne() can be reused by Batch Merge
// (POST /entityOperations/merge) — batch will open the collection once, loop
// over this helper, and collect results into a bulk write.
//
// Granularity is the top-level attribute (what the merge report tracks).
// "attributeCreated" and "attributeModified" become $set with the whole
// attribute wrapper as the value; "attributeDeleted" becomes $unset. This is
// already a big win over replacing the entire document, and it keeps the
// stored shape in sync with the in-memory target, which is what subsequent
// reads expect. A finer-grained diff at sub-attribute path level could be
// added later as an optimization; at that point the merge helper would need
// to emit path-level change records as well.
//

#include <string.h>                                   // strcmp, strlen

#include <mongoc/mongoc.h>                            // mongoc_collection_*, mongoc_cursor_*

#include "ktrace/kTrace.h"                            // KT_E
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjLookup.h"                           // kjLookup
#include "swRest/SwRestState.h"                       // swRest

#include "swNgsild/LdVocab.h"                         // LD_VOCAB_MODIFIED_AT, LD_VOCAB_CREATED_AT
#include "swNgsild/ldEntityMerge.h"                   // ldEntityMerge, LdMergeReport

#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, DB_ERR
#include "currentState/mongoc/mongocBsonToKjTree.h"   // mongocBsonToKjTree
#include "currentState/mongoc/mongocKjTreeToBson.h"   // mongocKjNodeAppend
#include "currentState/mongoc/mongocDotEscape.h"      // mongocEscapeDotsInKey
#include "currentState/mongoc/mongocEntityMerge.h"    // Own interface



// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocEntityMergeOne -
//
// -----------------------------------------------------------------------------
//
// mongocEntityMergeBuildUpdate - merge the fragment into `target` in memory
// and translate the merge report into a $set/$unset bson body.
//
// `target` must be the in-memory tree of the CURRENT document (the caller
// has done the fetch — either a single find_one or a $in batch fetch).
// Returns DB_OK if the update-doc was produced (even if empty — the merge
// legitimately touched nothing and the caller should skip the bulk op).
// Returns DB_OK with *noChangesP = true when there is nothing to write.
//
// On success: *updateDocOut is populated with { $set:..., $unset:... }.
// Caller is responsible for bson_destroy(updateDocOut).
//
int mongocEntityMergeBuildUpdate(KjNode*              target,
                                 KjNode*              fragmentDb,
                                 uint64_t             ts,
                                 LdMergeReport*       reportP,
                                 bson_t*              updateDocOut,
                                 bool*                noChangesOut)
{
  if (target == NULL || fragmentDb == NULL || updateDocOut == NULL)
    return DB_ERR;

  // Initialise updateDocOut up-front so the caller can always bson_destroy
  // it, even on early returns.
  bson_init(updateDocOut);

  if (noChangesOut != NULL)
    *noChangesOut = true;

  //
  // 1. Apply merge in memory. target and fragment are in the request arena
  //    (swRest.kjsonP) — grafted nodes need matching lifetime.
  //
  if (ldEntityFragmentApply(target, fragmentDb, reportP, ts, swRest.kjsonP) == false)
    return DB_OK;  // ldError already set; service routine sees problemType

  //
  // 2. Build a surgical update document from the merge report.
  //
  //    { $set:   { attrA: <wrapper>, ... modifiedAt, type, scope },
  //      $unset: { attrC: 1, attrD: 1 } }
  //
  bson_t setDoc;
  bson_t unsetDoc;
  bson_init(&setDoc);
  bson_init(&unsetDoc);

  bool hasSet   = false;
  bool hasUnset = false;

  if (reportP != NULL && reportP->changes != NULL)
  {
    for (KjNode* change = reportP->changes->value.firstChildP; change != NULL; change = change->next)
    {
      KjNode* attrNameP = kjLookup(change, "attr");
      KjNode* reasonP   = kjLookup(change, "reason");

      if (attrNameP == NULL || reasonP == NULL || attrNameP->type != KjString || reasonP->type != KjString)
        continue;

      const char* attrName = attrNameP->value.s;
      const char* reason   = reasonP->value.s;
      const char* escaped  = mongocEscapeDotsInKey(attrName);

      if (strcmp(reason, "attributeDeleted") == 0)
      {
        BSON_APPEND_INT32(&unsetDoc, escaped, 1);
        hasUnset = true;
      }
      else
      {
        KjNode* attrWrapper = kjLookup(target, attrName);
        if (attrWrapper == NULL)
          continue;

        mongocKjNodeAppend(&setDoc, escaped, attrWrapper);
        hasSet = true;
      }
    }
  }

  //
  // modifiedAt / type / scope refresh — whenever anything changed.
  //
  if (hasSet || hasUnset)
  {
    KjNode* modAtP = kjLookup(target, LD_VOCAB_MODIFIED_AT);
    if (modAtP != NULL && modAtP->type == KjInt)
    {
      mongocKjNodeAppend(&setDoc, LD_VOCAB_MODIFIED_AT, modAtP);
      hasSet = true;
    }

    KjNode* typeP = kjLookup(target, "type");
    if (typeP != NULL)
    {
      mongocKjNodeAppend(&setDoc, "type", typeP);
      hasSet = true;
    }

    KjNode* scopeP = kjLookup(target, LD_VOCAB_SCOPE);
    if (scopeP != NULL)
    {
      mongocKjNodeAppend(&setDoc, LD_VOCAB_SCOPE, scopeP);
      hasSet = true;
    }
  }

  if (hasSet)
    BSON_APPEND_DOCUMENT(updateDocOut, "$set", &setDoc);
  if (hasUnset)
    BSON_APPEND_DOCUMENT(updateDocOut, "$unset", &unsetDoc);

  bson_destroy(&setDoc);
  bson_destroy(&unsetDoc);

  if (noChangesOut != NULL)
    *noChangesOut = !(hasSet || hasUnset);

  return DB_OK;
}



// -----------------------------------------------------------------------------
//
// mongocEntityMergeOne -
//
int mongocEntityMergeOne(mongoc_collection_t* collP,
                         const char*          entityId,
                         KjNode*              fragmentDb,
                         uint64_t             ts,
                         LdMergeReport*       reportP,
                         KjNode**             targetPP)
{
  //
  // 1. Fetch current document by _id
  //
  bson_t filter;
  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", entityId);

  mongoc_cursor_t* cursorP = mongoc_collection_find_with_opts(collP, &filter, NULL, NULL);

  const bson_t* doc    = NULL;
  KjNode*       target = NULL;

  if (mongoc_cursor_next(cursorP, &doc))
  {
    target = mongocBsonToKjTree(&swRest.kalloc, doc);
  }
  else
  {
    bson_error_t cursorError;
    if (mongoc_cursor_error(cursorP, &cursorError))
    {
      KT_E("mongoc: entityMerge fetch failed: %s", cursorError.message);
      mongoc_cursor_destroy(cursorP);
      bson_destroy(&filter);
      return DB_ERR;
    }

    mongoc_cursor_destroy(cursorP);
    bson_destroy(&filter);
    return DB_NOT_FOUND;
  }

  mongoc_cursor_destroy(cursorP);

  //
  // 2. Apply merge + build surgical update (shared with batch-merge).
  //
  bson_t update;
  bool   noChanges = true;
  int    rc = mongocEntityMergeBuildUpdate(target, fragmentDb, ts, reportP, &update, &noChanges);
  if (rc != DB_OK)
  {
    bson_destroy(&filter);
    return rc;
  }

  int result = DB_OK;

  if (!noChanges)
  {
    bson_error_t err;
    bool ok = mongoc_collection_update_one(collP, &filter, &update, NULL, NULL, &err);

    if (!ok)
    {
      KT_E("mongoc: entityMerge update_one failed: %s", err.message);
      result = DB_ERR;
    }
  }

  bson_destroy(&update);
  bson_destroy(&filter);

  if (targetPP != NULL && result == DB_OK)
    *targetPP = target;

  return result;
}



// -----------------------------------------------------------------------------
//
// mongocEntityMerge -
//
int mongocEntityMerge(Tenant*        tenantP,
                      const char*    entityId,
                      KjNode*        fragmentDb,
                      uint64_t       ts,
                      LdMergeReport* reportP)
{
  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  int result = mongocEntityMergeOne(collP, entityId, fragmentDb, ts, reportP, NULL);

  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}

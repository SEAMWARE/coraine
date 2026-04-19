//
// FILE            mongocEntityAttrsSet.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// mongoc entityAttrsSet: fetch the current document, apply
// ldEntityAttrsSet in memory, then $set only the attribute wrappers
// ldEntityAttrsSet actually touched (plus the entity-level
// modifiedAt / type / scope). Append never deletes — no $unset.
//
// Writing only touched attrs (not the whole document) matters for
// mongoc, where each write is a wire op.
//

#include <string.h>                                    // strcmp, strlen

#include <mongoc/mongoc.h>                             // mongoc_collection_*, mongoc_cursor_*

#include "ktrace/kTrace.h"                             // KT_E
#include "kjson/KjNode.h"                              // KjNode
#include "kjson/kjLookup.h"                            // kjLookup
#include "swRest/SwRestState.h"                        // swRest

#include "swNgsild/LdVocab.h"                          // LD_VOCAB_MODIFIED_AT
#include "swNgsild/ldEntityAttrsSet.h"                 // ldEntityAttrsSet

#include "db/DbDriver.h"                               // DB_OK, DB_NOT_FOUND, DB_ERR
#include "currentState/mongoc/mongocBsonToKjTree.h"    // mongocBsonToKjTree
#include "currentState/mongoc/mongocKjTreeToBson.h"    // mongocKjNodeAppend
#include "currentState/mongoc/mongocDotEscape.h"       // mongocEscapeDotsInKey
#include "currentState/mongoc/mongocEntityAttrsSet.h"  // Own interface



extern mongoc_client_pool_t* poolP;



// -----------------------------------------------------------------------------
//
// mongocEntityAttrsSet -
//
int mongocEntityAttrsSet(Tenant*        tenantP,
                         const char*    entityId,
                         KjNode*        fragmentDb,
                         bool           overwriteScope,
                         uint64_t       ts,
                         LdMergeReport* reportP)
{
  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

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
    int rc = DB_NOT_FOUND;
    if (mongoc_cursor_error(cursorP, &cursorError))
    {
      KT_E("mongoc: entityAttrsSet fetch failed: %s", cursorError.message);
      rc = DB_ERR;
    }
    mongoc_cursor_destroy(cursorP);
    bson_destroy(&filter);
    mongoc_collection_destroy(collP);
    mongoc_client_pool_push(poolP, clientP);
    return rc;
  }

  mongoc_cursor_destroy(cursorP);

  //
  // 2. Apply append semantics in memory. Target + grafted fragment nodes
  //    share the request arena.
  //
  ldEntityAttrsSet(target, fragmentDb, overwriteScope, ts, reportP, swRest.kjsonP);

  //
  // 3. Build a surgical $set from the merge report. Append never deletes,
  //    so no $unset. Each touched attr is written as the whole wrapper.
  //
  bson_t update;
  bson_t setDoc;
  bson_init(&update);
  bson_init(&setDoc);

  bool hasSet = false;

  if (reportP != NULL && reportP->changes != NULL)
  {
    for (KjNode* change = reportP->changes->value.firstChildP; change != NULL; change = change->next)
    {
      KjNode* attrNameP = kjLookup(change, "attr");
      if (attrNameP == NULL || attrNameP->type != KjString)
        continue;

      const char* attrName    = attrNameP->value.s;
      const char* escaped     = mongocEscapeDotsInKey(attrName);
      KjNode*     attrWrapper = kjLookup(target, attrName);
      if (attrWrapper == NULL)
        continue;

      mongocKjNodeAppend(&setDoc, escaped, attrWrapper);
      hasSet = true;
    }
  }

  //
  // Refresh entity-level modifiedAt / type / scope when anything changed.
  // ldEntityAttrsSet bumps all three in memory; we mirror those onto
  // the update doc.
  //
  if (hasSet)
  {
    KjNode* modAtP = kjLookup(target, LD_VOCAB_MODIFIED_AT);
    if (modAtP != NULL && modAtP->type == KjInt)
      mongocKjNodeAppend(&setDoc, LD_VOCAB_MODIFIED_AT, modAtP);

    KjNode* typeP = kjLookup(target, "type");
    if (typeP != NULL)
      mongocKjNodeAppend(&setDoc, "type", typeP);

    KjNode* scopeP = kjLookup(target, LD_VOCAB_SCOPE);
    if (scopeP != NULL)
      mongocKjNodeAppend(&setDoc, LD_VOCAB_SCOPE, scopeP);
  }

  int result = DB_OK;

  if (hasSet)
  {
    BSON_APPEND_DOCUMENT(&update, "$set", &setDoc);

    bson_error_t err;
    bool ok = mongoc_collection_update_one(collP, &filter, &update, NULL, NULL, &err);

    if (!ok)
    {
      KT_E("mongoc: entityAttrsSet update_one failed: %s", err.message);
      result = DB_ERR;
    }
  }

  bson_destroy(&update);
  bson_destroy(&setDoc);
  bson_destroy(&filter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}

//
// FILE            mongocEntityAttrsSet.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// mongoc entityAttrsSet: fetch the current document, apply
// ldEntityAttrsSet in memory, then $set the touched wrappers and
// $unset any attrs ldEntityAttrsSet deleted (PATCH /attrs null-markers).
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

#include "db/DbDriver.h"                               // DB_OK, DB_NOT_FOUND, DB_ERR, DB_INVALID_GEOMETRY
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
  // 3. Build a surgical $set + $unset from the merge report.
  //    - "attributeDeleted" → $unset (attr is gone from target after merge)
  //    - anything else      → $set the whole wrapper from target
  //
  bson_t update;
  bson_t setDoc;
  bson_t unsetDoc;
  bson_init(&update);
  bson_init(&setDoc);
  bson_init(&unsetDoc);

  bool hasSet   = false;
  bool hasUnset = false;

  if (reportP != NULL && reportP->changes != NULL)
  {
    for (KjNode* change = reportP->changes->value.firstChildP; change != NULL; change = change->next)
    {
      KjNode* attrNameP = kjLookup(change, "attr");
      if (attrNameP == NULL || attrNameP->type != KjString)
        continue;

      KjNode*     reasonP  = kjLookup(change, "reason");
      const char* reason   = (reasonP != NULL && reasonP->type == KjString) ? reasonP->value.s : "";
      const char* attrName = attrNameP->value.s;

      // Entity-level type / scope changes are signalled in the report
      // so we know to bump them, but the always-write block below
      // appends them once to $set — appending here as well would make
      // mongo reject the bulk update with a "conflict at 'type'" /
      // "conflict at 'scope'" error.
      if (strcmp(attrName, "type")             == 0) { hasSet = true; continue; }
      if (strcmp(attrName, LD_VOCAB_SCOPE)     == 0) { hasSet = true; continue; }

      const char* escaped  = mongocEscapeDotsInKey(attrName);

      if (strcmp(reason, "attributeDeleted") == 0)
      {
        BSON_APPEND_UTF8(&unsetDoc, escaped, "");
        hasUnset = true;
        continue;
      }

      KjNode* attrWrapper = kjLookup(target, attrName);
      if (attrWrapper == NULL)
        continue;

      mongocKjNodeAppend(&setDoc, escaped, attrWrapper);
      hasSet = true;
    }
  }

  //
  // Refresh entity-level modifiedAt / type / scope when anything changed.
  // ldEntityAttrsSet bumps all three in memory; we mirror those onto
  // the $set portion.
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

  int result = DB_OK;

  if (hasSet || hasUnset)
  {
    if (hasSet)   BSON_APPEND_DOCUMENT(&update, "$set",   &setDoc);
    if (hasUnset) BSON_APPEND_DOCUMENT(&update, "$unset", &unsetDoc);

    bson_error_t err;
    bool ok = mongoc_collection_update_one(collP, &filter, &update, NULL, NULL, &err);

    if (!ok)
    {
      // "Can't extract geo keys" — a set GeoProperty value is well-formed JSON
      // but not a valid geometry. Surface as a client error (→ 400) rather than
      // a misleading 500. Mirrors mongocEntityCreate.
      if (strstr(err.message, "Can't extract geo keys") != NULL)
      {
        KT_E("mongoc: entityAttrsSet rejected by 2dsphere: %s", err.message);
        result = DB_INVALID_GEOMETRY;
      }
      else
      {
        KT_E("mongoc: entityAttrsSet update_one failed: %s", err.message);
        result = DB_ERR;
      }
    }
  }

  bson_destroy(&update);
  bson_destroy(&setDoc);
  bson_destroy(&unsetDoc);
  bson_destroy(&filter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}

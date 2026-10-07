//
// FILE            mongocEntityAttrsSet.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
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

#include "corLog/corLog.h"                             // COR_E
#include "corTree/CorNode.h"                           // CorNode
#include "corTree/corTreeLookup.h"                     // corTreeLookup
#include "corRest/CorRestState.h"                        // corRest

#include "corNgsild/LdVocab.h"                          // LD_VOCAB_MODIFIED_AT
#include "corNgsild/ldEntityAttrsSet.h"                 // ldEntityAttrsSet

#include "db/DbDriver.h"                               // DB_OK, DB_NOT_FOUND, DB_ERR, DB_INVALID_GEOMETRY
#include "currentState/mongoc/mongocBsonToTree.h"      // mongocEntityBsonToTree
#include "currentState/mongoc/mongocTreeToBson.h"      // mongocNodeAppend, mongocAttrAppend, mongocEntityCreatedAt
#include "currentState/mongoc/mongocDotEscape.h"       // mongocEscapeDotsInKey
#include "corNgsild/CorNgsild.h"                       // corNgsild (geoConflictAttr)
#include "currentState/mongoc/mongocGeoIndex.h"        // mongocGeoIndexEnsure
#include "currentState/mongoc/mongocEntityAttrsSet.h"  // Own interface



extern mongoc_client_pool_t* poolP;



// -----------------------------------------------------------------------------
//
// mongocEntityAttrsSet -
//
int mongocEntityAttrsSet(Tenant*        tenantP,
                         const char*    entityId,
                         CorNode*       fragmentDb,
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
  CorNode*      target = NULL;

  if (mongoc_cursor_next(cursorP, &doc))
  {
    target = mongocEntityBsonToTree(&corRest.kalloc, doc);
  }
  else
  {
    bson_error_t cursorError;
    int rc = DB_NOT_FOUND;
    if (mongoc_cursor_error(cursorP, &cursorError))
    {
      COR_E("mongoc: entityAttrsSet fetch failed: %s", cursorError.message);
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
  ldEntityAttrsSet(target, fragmentDb, overwriteScope, ts, reportP, corRest.kallocP);

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
    for (CorNode* change = reportP->changes->value.head; change != NULL; change = change->next)
    {
      CorNode* attrNameP = corTreeLookup(change, "attr");
      if (attrNameP == NULL || attrNameP->type != CorString)
        continue;

      CorNode*    reasonP  = corTreeLookup(change, "reason");
      const char* reason   = (reasonP != NULL && reasonP->type == CorString) ? reasonP->value.s : "";
      const char* attrName = attrNameP->value.s;

      // Entity-level type / scope changes are signalled in the report
      // so we know to bump them, but the always-write block below
      // appends them once to $set — appending here as well would make
      // mongo reject the bulk update with a "conflict at 'type'" /
      // "conflict at 'scope'" error.
      if (strcmp(attrName, "type")             == 0) { hasSet = true; continue; }

      //
      // ... with one exception: when the fragment deleted the scope (§ 5.4.1, the NGSI-LD Null),
      // the merged Entity carries none, so the refresh block has nothing to write and only an
      // $unset takes it out of the stored document.
      //
      if (strcmp(attrName, LD_VOCAB_SCOPE) == 0)
      {
        if (corTreeLookup(target, LD_VOCAB_SCOPE) == NULL)
        {
          BSON_APPEND_UTF8(&unsetDoc, mongocEscapeDotsInKey(attrName), "");
          hasUnset = true;
        }
        else
          hasSet = true;

        continue;
      }

      const char* escaped  = mongocEscapeDotsInKey(attrName);

      if (strcmp(reason, "attributeDeleted") == 0)
      {
        BSON_APPEND_UTF8(&unsetDoc, escaped, "");
        hasUnset = true;
        continue;
      }

      CorNode* attrWrapper = corTreeLookup(target, attrName);
      if (attrWrapper == NULL)
        continue;

      mongocAttrAppend(&setDoc, escaped, attrWrapper, mongocEntityCreatedAt(target));
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
    CorNode* modAtP = corTreeLookup(target, LD_VOCAB_MODIFIED_AT);
    if (modAtP != NULL && modAtP->type == CorInt)
    {
      mongocNodeAppend(&setDoc, LD_VOCAB_MODIFIED_AT, modAtP);
      hasSet = true;
    }

    CorNode* typeP = corTreeLookup(target, "type");
    if (typeP != NULL)
    {
      mongocNodeAppend(&setDoc, "type", typeP);
      hasSet = true;
    }

    CorNode* scopeP = corTreeLookup(target, LD_VOCAB_SCOPE);
    if (scopeP != NULL)
    {
      mongocNodeAppend(&setDoc, LD_VOCAB_SCOPE, scopeP);
      hasSet = true;
    }
  }

  int result = DB_OK;

  if (hasSet || hasUnset)
  {
    if (hasSet)   BSON_APPEND_DOCUMENT(&update, "$set",   &setDoc);
    if (hasUnset) BSON_APPEND_DOCUMENT(&update, "$unset", &unsetDoc);

    //
    // A set can introduce a new GeoProperty attribute — ensure its 2dsphere index
    // BEFORE the update, so a later georel=near / dist-sort query over it does not
    // fail for want of an index, and so a name already held as another type is
    // refused with the Entity untouched. Cached field paths cost a string compare.
    //
    const char* geoClashP = mongocGeoIndexEnsure(tenantP, fragmentDb, collP);

    bson_error_t err;
    if (geoClashP != NULL)
    {
      COR_E("mongoc: entityAttrsSet: '%s' is a GeoProperty here but already held as another type", geoClashP);
      corNgsild.geoConflictAttr = geoClashP;
      result = DB_GEO_TYPE_CONFLICT;
    }
    else if (!mongoc_collection_update_one(collP, &filter, &update, NULL, NULL, &err))
    {
      // "Can't extract geo keys" has two causes, separated only now that the write
      // has failed, so no ordinary append/set pays for the distinction: a name that
      // is geo-indexed here but set to another type is a clash of Attribute kinds
      // (→ 409); otherwise the geometry itself is one S2 will not take (→ 400).
      if (strstr(err.message, "Can't extract geo keys") != NULL)
      {
        const char* mixedP = mongocGeoIndexMixedName(tenantP, fragmentDb);
        if (mixedP != NULL)
        {
          COR_E("mongoc: entityAttrsSet: '%s' is held as a GeoProperty here and set to another type", mixedP);
          corNgsild.geoConflictAttr = mixedP;
          result = DB_GEO_TYPE_CONFLICT;
        }
        else
        {
          COR_E("mongoc: entityAttrsSet rejected by 2dsphere: %s", err.message);
          result = DB_INVALID_GEOMETRY;
        }
      }
      else
      {
        COR_E("mongoc: entityAttrsSet update_one failed: %s", err.message);
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

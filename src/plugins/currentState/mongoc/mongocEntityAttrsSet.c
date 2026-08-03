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
#include "swNgsild/SwNgsild.h"                          // swNgsild (geoConflictAttr)
#include "currentState/mongoc/mongocGeoIndex.h"        // mongocGeoIndexEnsure
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

      //
      // ... with one exception: when the fragment deleted the scope (§ 5.4.1, the NGSI-LD Null),
      // the merged Entity carries none, so the refresh block has nothing to write and only an
      // $unset takes it out of the stored document.
      //
      if (strcmp(attrName, LD_VOCAB_SCOPE) == 0)
      {
        if (kjLookup(target, LD_VOCAB_SCOPE) == NULL)
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
      KT_E("mongoc: entityAttrsSet: '%s' is a GeoProperty here but already held as another type", geoClashP);
      swNgsild.geoConflictAttr = geoClashP;
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
          KT_E("mongoc: entityAttrsSet: '%s' is held as a GeoProperty here and set to another type", mixedP);
          swNgsild.geoConflictAttr = mixedP;
          result = DB_GEO_TYPE_CONFLICT;
        }
        else
        {
          KT_E("mongoc: entityAttrsSet rejected by 2dsphere: %s", err.message);
          result = DB_INVALID_GEOMETRY;
        }
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

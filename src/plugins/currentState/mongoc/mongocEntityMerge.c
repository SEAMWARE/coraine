//
// FILE            mongocEntityMerge.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// mongoc change-set persistence for Merge Entity / Partial Attribute Update.
//
// The NGSI-LD merge itself (RFC 7396 / replace-append) is done by the broker
// against an in-memory tree fetched via db.entityRetrieve; the broker hands us
// the already-merged entity plus an LdMergeReport describing which top-level
// attributes were created/modified/deleted. This file only translates that
// report into a surgical $set/$unset and writes it.
//
// "attributeCreated" and "attributeModified" become $set with the whole
// (merged) attribute wrapper as the value; "attributeDeleted" becomes $unset.
// A PATCH that touches one attribute on a 2000-attribute entity transfers only
// that attribute plus the entity-level modifiedAt, not the whole document.
//
// mongocBuildSurgicalUpdate() is shared with the batch path
// (mongocEntityBulkChangesApply), which stages many of these updates into a
// single bulk operation.
//

#include <string.h>                                   // strcmp, strlen

#include <mongoc/mongoc.h>                            // mongoc_collection_*, mongoc_cursor_*

#include "ktrace/kTrace.h"                            // KT_E
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjLookup.h"                           // kjLookup

#include "swNgsild/LdVocab.h"                         // LD_VOCAB_MODIFIED_AT, LD_VOCAB_CREATED_AT
#include "swNgsild/ldEntityMerge.h"                   // LdMergeReport

#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, DB_ERR, DB_INVALID_GEOMETRY
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
// mongocBuildSurgicalUpdate - translate a merge report into a $set/$unset body.
//
// `mergedEntity` is the already-merged in-memory tree (the broker ran the merge
// engine); `reportP` lists which top-level attributes changed. The new attribute
// wrappers are read from `mergedEntity`. Appends into the caller-initialised
// `updateDocOut` ({ $set:{...}, $unset:{...} }). *noChangesOut is set true when
// the report produced nothing to write (caller should skip the DB write).
//
// No merge logic here — that lives in the broker (ldEntityMerge /
// ldEntityFragmentApply).
//
void mongocBuildSurgicalUpdate(KjNode*        mergedEntity,
                               LdMergeReport* reportP,
                               bson_t*        updateDocOut,
                               bool*          noChangesOut)
{
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
        KjNode* attrWrapper = kjLookup(mergedEntity, attrName);
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
    KjNode* modAtP = kjLookup(mergedEntity, LD_VOCAB_MODIFIED_AT);
    if (modAtP != NULL && modAtP->type == KjInt)
    {
      mongocKjNodeAppend(&setDoc, LD_VOCAB_MODIFIED_AT, modAtP);
      hasSet = true;
    }

    KjNode* typeP = kjLookup(mergedEntity, "type");
    if (typeP != NULL)
    {
      mongocKjNodeAppend(&setDoc, "type", typeP);
      hasSet = true;
    }

    KjNode* scopeP = kjLookup(mergedEntity, LD_VOCAB_SCOPE);
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
}



// -----------------------------------------------------------------------------
//
// mongocEntityChangesApply - persist a merged single entity (DB driver entry)
//
// The broker has already merged `mergedEntity` in memory and produced
// `reportP`. Build the surgical update and run one update_one. An empty report
// (the merge produced no net change) writes nothing and returns DB_OK.
//
int mongocEntityChangesApply(Tenant* tenantP, const char* entityId,
                             KjNode* mergedEntity, LdMergeReport* reportP)
{
  bson_t update;
  bson_init(&update);

  bool noChanges = true;
  mongocBuildSurgicalUpdate(mergedEntity, reportP, &update, &noChanges);

  int result = DB_OK;

  if (!noChanges)
  {
    mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
    mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

    bson_t filter;
    bson_init(&filter);
    BSON_APPEND_UTF8(&filter, "_id", entityId);

    bson_error_t err;
    if (!mongoc_collection_update_one(collP, &filter, &update, NULL, NULL, &err))
    {
      // "Can't extract geo keys" — a merged GeoProperty value is well-formed
      // JSON but not a valid geometry (e.g. a wholesale-replaced value with no
      // coordinates). Surface as a client error so the caller maps it to 400
      // rather than a misleading 500. Mirrors mongocEntityCreate.
      if (strstr(err.message, "Can't extract geo keys") != NULL)
      {
        KT_E("mongoc: entityChangesApply rejected by 2dsphere: %s", err.message);
        result = DB_INVALID_GEOMETRY;
      }
      else
      {
        KT_E("mongoc: entityChangesApply update_one failed: %s", err.message);
        result = DB_ERR;
      }
    }

    bson_destroy(&filter);
    mongoc_collection_destroy(collP);
    mongoc_client_pool_push(poolP, clientP);
  }

  bson_destroy(&update);

  return result;
}

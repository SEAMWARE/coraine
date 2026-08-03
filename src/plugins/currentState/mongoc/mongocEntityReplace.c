//
// FILE            mongocEntityReplace.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <mongoc/mongoc.h>                            // mongoc_*, bson_*

#include "ktrace/kTrace.h"                            // KT_E
#include "kjson/KjNode.h"                             // KjNode
#include "swRest/SwRestState.h"                       // swRest

#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, DB_ERR
#include "currentState/mongoc/mongocKjTreeToBson.h"   // mongocKjTreeToBson
#include "currentState/mongoc/mongocBsonToKjTree.h"   // mongocBsonToKjTree
#include "swNgsild/SwNgsild.h"                          // swNgsild (geoConflictAttr)
#include "currentState/mongoc/mongocGeoIndex.h"       // mongocGeoIndexEnsure
#include "currentState/mongoc/mongocEntityReplace.h"  // Own interface



// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocEntityReplace -
//
int mongocEntityReplace(Tenant* tenantP, const char* entityId, KjNode* newEntityP, KjNode** oldEntityPP)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  //
  // Filter: { "_id": entityId }
  //
  bson_t filter;
  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", entityId);

  //
  // Replacement document: the full new entity in BSON form
  //
  bson_t replacement;
  mongocKjTreeToBson(newEntityP, &replacement);

  //
  // find_and_modify: atomic "find by _id, replace with new doc, return pre-image".
  // The default return is the pre-image (without set_new(true) it's the old doc),
  // which is exactly what the caller needs for the notification payload's
  // previousValue fields and for logging / diagnostics.
  //
  mongoc_find_and_modify_opts_t* opts = mongoc_find_and_modify_opts_new();
  mongoc_find_and_modify_opts_set_update(opts, &replacement);

  //
  // A replacement may introduce a GeoProperty attribute — ensure its 2dsphere index
  // BEFORE replacing, so a name already held as another type is refused with the
  // Entity untouched. Cached field paths cost a string compare. (Removal of now
  // stale indexes is not tracked here, as it is not anywhere else in the codebase.)
  //
  const char* geoClashP = mongocGeoIndexEnsure(tenantP, newEntityP, collP);

  bson_t       reply;
  bson_error_t error;
  bool         ok     = false;
  int          result = DB_OK;

  if (geoClashP != NULL)
  {
    KT_E("mongoc: entityReplace: '%s' is a GeoProperty here but already held as another type", geoClashP);
    swNgsild.geoConflictAttr = geoClashP;
    bson_init(&reply);
    result = DB_GEO_TYPE_CONFLICT;
  }
  else
    ok = mongoc_collection_find_and_modify_with_opts(collP, &filter, opts, &reply, &error);

  if (result == DB_GEO_TYPE_CONFLICT)
  {
    if (oldEntityPP != NULL)
      *oldEntityPP = NULL;
  }
  else if (!ok)
  {
    //
    // Same two causes as everywhere else, told apart only on the failing path: a
    // name geo-indexed here but replaced with another type is a clash of Attribute
    // kinds (→ 409); otherwise S2 refused the geometry itself (→ 400). Replace used
    // to lump both in with DB_ERR, which is where the 500 came from.
    //
    if (strstr(error.message, "Can't extract geo keys") != NULL)
    {
      const char* mixedP = mongocGeoIndexMixedName(tenantP, newEntityP);
      if (mixedP != NULL)
      {
        KT_E("mongoc: entityReplace: '%s' is held as a GeoProperty here and replaced with another type", mixedP);
        swNgsild.geoConflictAttr = mixedP;
        result = DB_GEO_TYPE_CONFLICT;
      }
      else
      {
        KT_E("mongoc: entityReplace rejected by 2dsphere: %s", error.message);
        result = DB_INVALID_GEOMETRY;
      }
    }
    else
    {
      KT_E("mongoc: entityReplace failed: %s", error.message);
      result = DB_ERR;
    }

    if (oldEntityPP != NULL)
      *oldEntityPP = NULL;
  }
  else
  {
    //
    // The server returns { "value": <old_doc> | null, "lastErrorObject": {...}, ... }.
    // A null "value" means no document matched — DB_NOT_FOUND.
    //
    bson_iter_t iter;
    if (bson_iter_init_find(&iter, &reply, "value") && BSON_ITER_HOLDS_DOCUMENT(&iter))
    {
      uint32_t      len;
      const uint8_t* data;
      bson_iter_document(&iter, &len, &data);

      bson_t oldDoc;
      if (bson_init_static(&oldDoc, data, len))
      {
        if (oldEntityPP != NULL)
          *oldEntityPP = mongocBsonToKjTree(&swRest.kalloc, &oldDoc);
      }
      else if (oldEntityPP != NULL)
      {
        *oldEntityPP = NULL;
      }
    }
    else
    {
      result = DB_NOT_FOUND;
      if (oldEntityPP != NULL)
        *oldEntityPP = NULL;
    }
  }

  bson_destroy(&reply);
  bson_destroy(&replacement);
  bson_destroy(&filter);
  mongoc_find_and_modify_opts_destroy(opts);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}

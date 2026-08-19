//
// FILE            mongocEntityCreate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <string.h>                                  // strstr
#include <mongoc/mongoc.h>                           // mongoc_collection_t, mongoc_collection_insert_one

#include "ktrace/kTrace.h"                               // KT_E
#include "kjson/KjNode.h"                            // KjNode

#include "db/DbDriver.h"                             // DB_OK, DB_ALREADY_EXISTS, DB_ERR, DB_INVALID_GEOMETRY
#include "currentState/mongoc/mongocKjTreeToBson.h"               // mongocKjTreeToBson
#include "corNgsild/CorNgsild.h"                          // corNgsild (geoConflictAttr)
#include "currentState/mongoc/mongocGeoIndex.h"                   // mongocGeoIndexEnsure
#include "currentState/mongoc/mongocEntityCreate.h"               // Own interface



// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocEntityCreate -
//
int mongocEntityCreate(Tenant* tenantP, const char* entityId, KjNode* entityP)
{
  //
  // Convert KjTree to BSON
  //
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");
  bson_t                bson;

  //
  // Ensure the 2dsphere indexes this entity's GeoProperties need, BEFORE inserting:
  // the build is what discovers that the name is already in use as something else,
  // and doing it first means the failing case stores nothing.
  //
  const char* geoClashP = mongocGeoIndexEnsure(tenantP, entityP, collP);
  if (geoClashP != NULL)
  {
    mongoc_collection_destroy(collP);
    mongoc_client_pool_push(poolP, clientP);
    KT_E("mongoc: entityCreate: '%s' is a GeoProperty here but already held as another type", geoClashP);
    corNgsild.geoConflictAttr = geoClashP;
    return DB_GEO_TYPE_CONFLICT;
  }

  mongocKjTreeToBson(entityP, &bson);

  //
  // Insert
  //
  bson_error_t error;
  bool ok = mongoc_collection_insert_one(collP, &bson, NULL, NULL, &error);

  bson_destroy(&bson);

  if (!ok)
  {
    mongoc_collection_destroy(collP);
    mongoc_client_pool_push(poolP, clientP);

    if (error.code == 11000)
      return DB_ALREADY_EXISTS;

    // "Can't extract geo keys" is mongo's 2dsphere/S2 rejection — the
    // GeoProperty value is well-formed JSON but S2 considers the polygon
    // self-intersecting / degenerate. Surface as a client error rather
    // than a generic 500, so the caller can map it to 400 BadRequestData.
    if (strstr(error.message, "Can't extract geo keys") != NULL)
    {
      //
      // Two causes, told apart only now that the write has failed — the ordinary
      // write pays nothing for this. An Attribute of this payload that is
      // geo-indexed in the tenant but is NOT a GeoProperty here is a clash of
      // Attribute kinds (409); anything else really is a geometry S2 refuses (400).
      //
      const char* mixedP = mongocGeoIndexMixedName(tenantP, entityP);
      if (mixedP != NULL)
      {
        KT_E("mongoc: entityCreate: '%s' is held as a GeoProperty here and written as another type", mixedP);
        corNgsild.geoConflictAttr = mixedP;
        return DB_GEO_TYPE_CONFLICT;
      }

      KT_E("mongoc: entityCreate rejected by 2dsphere: %s", error.message);
      return DB_INVALID_GEOMETRY;
    }

    KT_E("mongoc: entityCreate failed: %s", error.message);
    return DB_ERR;
  }


  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return DB_OK;
}

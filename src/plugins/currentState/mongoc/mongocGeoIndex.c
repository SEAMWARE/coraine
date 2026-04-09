//
// FILE            mongocGeoIndex.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Geo-index management for MongoDB:
//   - At startup, an aggregation pipeline discovers all (attrName, datasetKey) pairs
//     that are GeoProperty in the existing entities, and creates 2dsphere indexes.
//   - On entity creation, the entity tree is scanned and new indexes are created
//     for any GeoProperty instances not yet indexed.
//
// This avoids hardcoding any specific attribute names (like "location") - the indexes
// are driven entirely by the data.
//
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // malloc
#include <string.h>                                  // strcmp, strdup

#include <mongoc/mongoc.h>                           // mongoc_collection_t, ...

#include "ktrace/kTrace.h"                               // KT_I, KT_E, KT_V
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjLookup.h"                          // kjLookup
#include "swNgsild/LdVocab.h"                        // LD_VOCAB_SCOPE, LD_VOCAB_CREATED_AT, LD_VOCAB_MODIFIED_AT
#include "swNgsild/ldTypes.h"                        // ldAttrTypeFromString, LdAttrGeoProperty

#include "db/Tenant.h"                                             // Tenant
#include "currentState/mongoc/mongocDotEscape.h"                   // mongocEscapeDotsInKey
#include "currentState/mongoc/mongocGeoIndex.h"                    // Own interface



// -----------------------------------------------------------------------------
//
// Per-tenant geo index cache - stored in tenantP->pluginData
//
// Each (attrName, datasetKey) combination has a unique field path:
//   "<escapedAttr>.<escapedDatasetKey>.value"
//
#define GEO_INDEX_CACHE_MAX  512

typedef struct MongocGeoCache
{
  char*  fieldPaths[GEO_INDEX_CACHE_MAX];
  int    count;
} MongocGeoCache;


static MongocGeoCache* geoCacheGet(Tenant* tenantP)
{
  if (tenantP->pluginData == NULL)
  {
    tenantP->pluginData = malloc(sizeof(MongocGeoCache));
    memset(tenantP->pluginData, 0, sizeof(MongocGeoCache));
  }

  return (MongocGeoCache*) tenantP->pluginData;
}


static bool geoIndexCacheLookup(Tenant* tenantP, const char* fieldPath)
{
  MongocGeoCache* cacheP = geoCacheGet(tenantP);

  for (int i = 0; i < cacheP->count; i++)
    if (strcmp(cacheP->fieldPaths[i], fieldPath) == 0)
      return true;

  return false;
}

static void geoIndexCacheAdd(Tenant* tenantP, const char* fieldPath)
{
  MongocGeoCache* cacheP = geoCacheGet(tenantP);

  if (cacheP->count < GEO_INDEX_CACHE_MAX)
    cacheP->fieldPaths[cacheP->count++] = strdup(fieldPath);
  else
    KT_E("mongoc: geo index cache full (%d entries) for db '%s', cannot track '%s'", GEO_INDEX_CACHE_MAX, tenantP->dbName, fieldPath);
}



// -----------------------------------------------------------------------------
//
// geoIndexCreate - create a 2dsphere index on a specific field path and cache it
//
static void geoIndexCreate(Tenant* tenantP, mongoc_collection_t* collP, const char* fieldPath)
{
  bson_t geoKeys;
  bson_init(&geoKeys);
  bson_append_utf8(&geoKeys, fieldPath, -1, "2dsphere", 8);

  bson_error_t           error;
  mongoc_index_model_t*  indexP = mongoc_index_model_new(&geoKeys, NULL);

  if (mongoc_collection_create_indexes_with_opts(collP, &indexP, 1, NULL, NULL, &error))
    KT_I("mongoc: ensured 2dsphere index on '%s' for db '%s'", fieldPath, tenantP->dbName);
  else
    KT_E("mongoc: failed to create 2dsphere index on '%s' for db '%s': %s", fieldPath, tenantP->dbName, error.message);

  mongoc_index_model_destroy(indexP);
  bson_destroy(&geoKeys);

  geoIndexCacheAdd(tenantP, fieldPath);
}



// -----------------------------------------------------------------------------
//
// mongocGeoIndexInit - discover GeoProperty attributes in existing entities via aggregation
//
void mongocGeoIndexInit(Tenant* tenantP, mongoc_collection_t* collP)
{
  //
  // Build pipeline as a JSON string
  //
  const char* pipelineJson =
    "{ \"pipeline\": ["
    "  { \"$project\": { \"_id\": 0, \"attrs\": { \"$objectToArray\": \"$$ROOT\" } } },"
    "  { \"$unwind\": \"$attrs\" },"
    "  { \"$match\": { \"attrs.v\": { \"$type\": \"object\" } } },"
    "  { \"$project\": { \"attr\": \"$attrs.k\", \"ds\": { \"$objectToArray\": \"$attrs.v\" } } },"
    "  { \"$unwind\": \"$ds\" },"
    "  { \"$match\": { \"ds.v.type\": { \"$in\": [ \"GeoProperty\", \"https://uri.etsi.org/ngsi-ld/GeoProperty\" ] } } },"
    "  { \"$group\": { \"_id\": { \"attr\": \"$attr\", \"ds\": \"$ds.k\" } } }"
    "] }";

  bson_error_t  error;
  bson_t*       pipelineP = bson_new_from_json((const uint8_t*) pipelineJson, -1, &error);

  if (pipelineP == NULL)
  {
    KT_E("mongoc: failed to parse geo index pipeline: %s", error.message);
    return;
  }

  mongoc_cursor_t* cursorP = mongoc_collection_aggregate(collP, MONGOC_QUERY_NONE, pipelineP, NULL, NULL);

  //
  // Each result document: { "_id": { "attr": "<expandedAttrName>", "ds": "<datasetKey>" } }
  //
  const bson_t* doc;
  int           indexCount = 0;

  while (mongoc_cursor_next(cursorP, &doc))
  {
    bson_iter_t iter;
    bson_iter_t idIter;

    if (!bson_iter_init_find(&iter, doc, "_id") || !BSON_ITER_HOLDS_DOCUMENT(&iter))
      continue;

    const uint8_t*  idData;
    uint32_t        idLen;
    bson_t          idDoc;

    bson_iter_document(&iter, &idLen, &idData);
    bson_init_static(&idDoc, idData, idLen);

    const char* attrName   = NULL;
    const char* datasetKey = NULL;

    if (bson_iter_init_find(&idIter, &idDoc, "attr") && BSON_ITER_HOLDS_UTF8(&idIter))
      attrName = bson_iter_utf8(&idIter, NULL);

    if (bson_iter_init_find(&idIter, &idDoc, "ds") && BSON_ITER_HOLDS_UTF8(&idIter))
      datasetKey = bson_iter_utf8(&idIter, NULL);

    if (attrName == NULL || datasetKey == NULL)
      continue;

    // Build field path - mongocEscapeDotsInKey uses a thread-local buffer, so copy first result
    char escapedAttrBuf[512];
    snprintf(escapedAttrBuf, sizeof(escapedAttrBuf), "%s", mongocEscapeDotsInKey(attrName));

    const char* escapedDsKey = mongocEscapeDotsInKey(datasetKey);

    char fieldPath[1024];
    snprintf(fieldPath, sizeof(fieldPath), "%s.%s.value", escapedAttrBuf, escapedDsKey);

    if (!geoIndexCacheLookup(tenantP, fieldPath))
    {
      geoIndexCreate(tenantP, collP, fieldPath);
      indexCount++;
    }
  }

  if (mongoc_cursor_error(cursorP, &error))
    KT_E("mongoc: geo index scan cursor error: %s", error.message);

  mongoc_cursor_destroy(cursorP);
  bson_destroy(pipelineP);

  if (indexCount > 0)
    KT_I("mongoc: created %d 2dsphere geo-indexes from existing data", indexCount);
}



// -----------------------------------------------------------------------------
//
// isEntityKeyword - check if a field name is an entity-level keyword (not an attribute)
//
static bool isEntityKeyword(const char* name)
{
  if (strcmp(name, "_id")                 == 0)  return true;
  if (strcmp(name, "id")                  == 0)  return true;
  if (strcmp(name, "type")                == 0)  return true;
  if (strcmp(name, "@context")            == 0)  return true;
  if (strcmp(name, LD_VOCAB_SCOPE)        == 0)  return true;
  if (strcmp(name, LD_VOCAB_CREATED_AT)   == 0)  return true;
  if (strcmp(name, LD_VOCAB_MODIFIED_AT)  == 0)  return true;

  return false;
}



// -----------------------------------------------------------------------------
//
// isGeoPropertyInstance - check if a dataset instance has type "GeoProperty"
//
static bool isGeoPropertyInstance(KjNode* instP)
{
  if (instP == NULL || instP->type != KjObject)
    return false;

  KjNode* typeP = kjLookup(instP, "type");

  if (typeP != NULL && typeP->type == KjString && ldAttrTypeFromString(typeP->value.s) == LdAttrGeoProperty)
    return true;

  return false;
}



// -----------------------------------------------------------------------------
//
// mongocGeoIndexEnsure - scan a newly created entity for GeoProperty attributes
//
// Called after entity insertion. Creates 2dsphere indexes for any (attr, datasetKey)
// combinations not yet in the cache.
//
void mongocGeoIndexEnsure(Tenant* tenantP, KjNode* entityP, mongoc_collection_t* collP)
{
  for (KjNode* childP = entityP->value.firstChildP; childP != NULL; childP = childP->next)
  {
    if (childP->type != KjObject || childP->name == NULL || isEntityKeyword(childP->name))
      continue;

    for (KjNode* instP = childP->value.firstChildP; instP != NULL; instP = instP->next)
    {
      if (!isGeoPropertyInstance(instP))
        continue;

      char escapedAttrBuf[512];
      snprintf(escapedAttrBuf, sizeof(escapedAttrBuf), "%s", mongocEscapeDotsInKey(childP->name));

      const char* escapedDsKey = mongocEscapeDotsInKey(instP->name);

      char fieldPath[1024];
      snprintf(fieldPath, sizeof(fieldPath), "%s.%s.value", escapedAttrBuf, escapedDsKey);

      if (geoIndexCacheLookup(tenantP, fieldPath))
        continue;

      geoIndexCreate(tenantP, collP, fieldPath);
    }
  }
}

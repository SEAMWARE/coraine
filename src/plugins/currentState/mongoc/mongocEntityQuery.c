//
// FILE            mongocEntityQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <mongoc/mongoc.h>                           // mongoc_collection_t, mongoc_collection_find_with_opts
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // strtod
#include <string.h>                                  // strlen, strcmp

#include "ktrace/kTrace.h"                               // KT_E
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjChildAdd
#include "kjson/kjBufferCreate.h"                    // kjBufferCreate
#include "swRest/SwRestState.h"                      // swRest
#include "swNgsild/LdVocab.h"                        // LD_VOCAB_SCOPE, LD_VOCAB_CREATED_AT
#include "swNgsild/LdGeoRel.h"                      // LdGeoRel, LdGeoRelType
#include "swNgsild/LdQ.h"                           // LdQNode, LdQTermNode, ...
#include "swNgsild/LdScopeExpr.h"                    // LdScopeExpr
#include "swNgsild/ldScopeMatch.h"                    // ldScopeToRegex

#include "db/DbDriver.h"                             // DB_OK, DB_ERR
#include "currentState/mongoc/mongocDotEscape.h"                  // mongocEscapeDotsInKey
#include "currentState/mongoc/mongocBsonToKjTree.h"               // mongocBsonToKjTree
#include "currentState/mongoc/mongocEntityQuery.h"                // Own interface




// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// bsonInArray - append a $in array for a NULL-terminated string vector
//
static void bsonInArray(bson_t* filterP, const char* field, char** valV)
{
  bson_t in;
  bson_t array;

  bson_append_document_begin(filterP, field, -1, &in);
  bson_append_array_begin(&in, "$in", 3, &array);

  for (int ix = 0; valV[ix] != NULL; ix++)
  {
    char key[16];
    int  keyLen = snprintf(key, sizeof(key), "%d", ix);
    bson_append_utf8(&array, key, keyLen, valV[ix], -1);
  }

  bson_append_array_end(&in, &array);
  bson_append_document_end(filterP, &in);
}




// -----------------------------------------------------------------------------
//
// bsonAppendScopeFilter - build BSON filter for scopeQ expression
//
static void bsonAppendScopeFilter(bson_t* filterP, LdScopeExpr* scopeExpr)
{
  char        regexBuf[512];
  const char* scopeKey = mongocEscapeDotsInKey(LD_VOCAB_SCOPE);

  //
  // Simple case: one group with one pattern
  //
  if (scopeExpr->groupCount == 1 && scopeExpr->groupV[0].count == 1)
  {
    ldScopeToRegex(scopeExpr->groupV[0].scopeV[0], regexBuf, sizeof(regexBuf));

    bson_t regexDoc;
    bson_append_document_begin(filterP, scopeKey, -1, &regexDoc);
    bson_append_regex(&regexDoc, "$regex", 6, regexBuf, "");
    bson_append_document_end(filterP, &regexDoc);
    return;
  }

  //
  // Simple case: all groups have count==1 - pure OR, use $or with $regex
  //
  if (scopeExpr->isSimple)
  {
    bson_t orArray;
    bson_append_array_begin(filterP, "$or", 3, &orArray);

    for (int gix = 0; gix < scopeExpr->groupCount; gix++)
    {
      char key[16];
      int  keyLen = snprintf(key, sizeof(key), "%d", gix);

      ldScopeToRegex(scopeExpr->groupV[gix].scopeV[0], regexBuf, sizeof(regexBuf));

      bson_t orElem;
      bson_append_document_begin(&orArray, key, keyLen, &orElem);

      bson_t regexDoc;
      bson_append_document_begin(&orElem, scopeKey, -1, &regexDoc);
      bson_append_regex(&regexDoc, "$regex", 6, regexBuf, "");
      bson_append_document_end(&orElem, &regexDoc);

      bson_append_document_end(&orArray, &orElem);
    }

    bson_append_array_end(filterP, &orArray);
    return;
  }

  //
  // Complex case: OR of AND groups
  //
  bson_t orArray;
  bson_append_array_begin(filterP, "$or", 3, &orArray);

  for (int gix = 0; gix < scopeExpr->groupCount; gix++)
  {
    char key[16];
    int  keyLen = snprintf(key, sizeof(key), "%d", gix);

    LdScopeGroup* grp = &scopeExpr->groupV[gix];
    bson_t        orElem;
    bson_append_document_begin(&orArray, key, keyLen, &orElem);

    if (grp->count == 1)
    {
      ldScopeToRegex(grp->scopeV[0], regexBuf, sizeof(regexBuf));

      bson_t regexDoc;
      bson_append_document_begin(&orElem, scopeKey, -1, &regexDoc);
      bson_append_regex(&regexDoc, "$regex", 6, regexBuf, "");
      bson_append_document_end(&orElem, &regexDoc);
    }
    else
    {
      bson_t allDoc;
      bson_t allArray;

      bson_append_document_begin(&orElem, scopeKey, -1, &allDoc);
      bson_append_array_begin(&allDoc, "$all", 4, &allArray);

      for (int six = 0; six < grp->count; six++)
      {
        char skey[16];
        int  skeyLen = snprintf(skey, sizeof(skey), "%d", six);

        ldScopeToRegex(grp->scopeV[six], regexBuf, sizeof(regexBuf));

        bson_t elemMatch;
        bson_t elemMatchInner;

        bson_append_document_begin(&allArray, skey, skeyLen, &elemMatch);
        bson_append_document_begin(&elemMatch, "$elemMatch", 10, &elemMatchInner);
        bson_append_regex(&elemMatchInner, "$regex", 6, regexBuf, "");
        bson_append_document_end(&elemMatch, &elemMatchInner);
        bson_append_document_end(&allArray, &elemMatch);
      }

      bson_append_array_end(&allDoc, &allArray);
      bson_append_document_end(&orElem, &allDoc);
    }

    bson_append_document_end(&orArray, &orElem);
  }

  bson_append_array_end(filterP, &orArray);
}



// -----------------------------------------------------------------------------
//
// bsonAppendQTerm - build BSON filter for a single Q term
//
static void bsonAppendQTerm(bson_t* docP, LdQTerm* term)
{
  //
  // Existence check
  //
  if (term->op == LdQExists)
  {
    const char* escapedAttr = mongocEscapeDotsInKey(term->attr);

    bson_t existsDoc;
    bson_append_document_begin(docP, escapedAttr, -1, &existsDoc);
    bson_append_bool(&existsDoc, "$exists", 7, true);
    bson_append_document_end(docP, &existsDoc);
    return;
  }

  //
  // Build field path: <escapedAttr>.@none.value
  //
  char path[1024];
  snprintf(path, sizeof(path), "%s.@none.value", mongocEscapeDotsInKey(term->attr));

  //
  // Simple equality / inequality with single value
  //
  if (term->valueType == LdQNumber)
  {
    if (term->op == LdQEqual)
    {
      bson_append_double(docP, path, -1, term->value.n);
    }
    else
    {
      const char* mongoOp = NULL;

      switch (term->op)
      {
      case LdQUnequal:   mongoOp = "$ne";  break;
      case LdQGreater:   mongoOp = "$gt";  break;
      case LdQLess:      mongoOp = "$lt";  break;
      case LdQGreaterEq: mongoOp = "$gte"; break;
      case LdQLessEq:    mongoOp = "$lte"; break;
      default:           mongoOp = "$eq";  break;
      }

      bson_t opDoc;
      bson_append_document_begin(docP, path, -1, &opDoc);
      bson_append_double(&opDoc, mongoOp, -1, term->value.n);
      bson_append_document_end(docP, &opDoc);
    }
  }
  else if (term->valueType == LdQString)
  {
    if (term->op == LdQEqual)
    {
      bson_append_utf8(docP, path, -1, term->value.s, -1);
    }
    else if (term->op == LdQUnequal)
    {
      bson_t opDoc;
      bson_append_document_begin(docP, path, -1, &opDoc);
      bson_append_utf8(&opDoc, "$ne", 3, term->value.s, -1);
      bson_append_document_end(docP, &opDoc);
    }
    else if (term->op == LdQPattern)
    {
      bson_t opDoc;
      bson_append_document_begin(docP, path, -1, &opDoc);
      bson_append_regex(&opDoc, "$regex", 6, term->value.s, "");
      bson_append_document_end(docP, &opDoc);
    }
    else if (term->op == LdQNotPattern)
    {
      bson_t notDoc;
      bson_t regexDoc;
      bson_append_document_begin(docP, path, -1, &notDoc);
      bson_append_document_begin(&notDoc, "$not", 4, &regexDoc);
      bson_append_regex(&regexDoc, "$regex", 6, term->value.s, "");
      bson_append_document_end(&notDoc, &regexDoc);
      bson_append_document_end(docP, &notDoc);
    }
    else
    {
      const char* mongoOp = NULL;

      switch (term->op)
      {
      case LdQGreater:   mongoOp = "$gt";  break;
      case LdQLess:      mongoOp = "$lt";  break;
      case LdQGreaterEq: mongoOp = "$gte"; break;
      case LdQLessEq:    mongoOp = "$lte"; break;
      default:           mongoOp = "$eq";  break;
      }

      bson_t opDoc;
      bson_append_document_begin(docP, path, -1, &opDoc);
      bson_append_utf8(&opDoc, mongoOp, -1, term->value.s, -1);
      bson_append_document_end(docP, &opDoc);
    }
  }
  else if (term->valueType == LdQBool)
  {
    if (term->op == LdQEqual)
    {
      bson_append_bool(docP, path, -1, term->value.b);
    }
    else if (term->op == LdQUnequal)
    {
      bson_t opDoc;
      bson_append_document_begin(docP, path, -1, &opDoc);
      bson_append_bool(&opDoc, "$ne", 3, term->value.b);
      bson_append_document_end(docP, &opDoc);
    }
  }
  else if (term->valueType == LdQDateTime)
  {
    if (term->op == LdQEqual)
    {
      bson_append_utf8(docP, path, -1, term->value.s, -1);
    }
    else
    {
      const char* mongoOp = NULL;

      switch (term->op)
      {
      case LdQUnequal:   mongoOp = "$ne";  break;
      case LdQGreater:   mongoOp = "$gt";  break;
      case LdQLess:      mongoOp = "$lt";  break;
      case LdQGreaterEq: mongoOp = "$gte"; break;
      case LdQLessEq:    mongoOp = "$lte"; break;
      default:           mongoOp = "$eq";  break;
      }

      bson_t opDoc;
      bson_append_document_begin(docP, path, -1, &opDoc);
      bson_append_utf8(&opDoc, mongoOp, -1, term->value.s, -1);
      bson_append_document_end(docP, &opDoc);
    }
  }
  else if (term->valueType == LdQRange)
  {
    bson_t rangeDoc;
    bson_append_document_begin(docP, path, -1, &rangeDoc);
    bson_append_double(&rangeDoc, "$gte", 4, term->value.numRange.lo);
    bson_append_double(&rangeDoc, "$lte", 4, term->value.numRange.hi);
    bson_append_document_end(docP, &rangeDoc);
  }
  else if (term->valueType == LdQDateRange)
  {
    bson_t rangeDoc;
    bson_append_document_begin(docP, path, -1, &rangeDoc);
    bson_append_utf8(&rangeDoc, "$gte", 4, term->value.dateRange.lo, -1);
    bson_append_utf8(&rangeDoc, "$lte", 4, term->value.dateRange.hi, -1);
    bson_append_document_end(docP, &rangeDoc);
  }
  else if (term->valueType == LdQValueList)
  {
    const char* mongoOp = (term->op == LdQEqual) ? "$in" : "$nin";
    int         mongoOpLen = (term->op == LdQEqual) ? 3 : 4;

    bson_t inDoc;
    bson_t array;
    bson_append_document_begin(docP, path, -1, &inDoc);
    bson_append_array_begin(&inDoc, mongoOp, mongoOpLen, &array);

    for (int i = 0; i < term->value.list.count; i++)
    {
      char key[16];
      int  keyLen = snprintf(key, sizeof(key), "%d", i);

      if (term->value.list.itemType == LdQNumber)
        bson_append_double(&array, key, keyLen, strtod(term->value.list.values[i], NULL));
      else if (term->value.list.itemType == LdQBool)
        bson_append_bool(&array, key, keyLen, strcmp(term->value.list.values[i], "true") == 0);
      else
        bson_append_utf8(&array, key, keyLen, term->value.list.values[i], -1);
    }

    bson_append_array_end(&inDoc, &array);
    bson_append_document_end(docP, &inDoc);
  }
}



// -----------------------------------------------------------------------------
//
// bsonAppendQNode - recursively build BSON filter from Q expression tree
//
static void bsonAppendQNode(bson_t* docP, LdQNode* nodeP)
{
  if (nodeP->type == LdQTermNode)
  {
    bsonAppendQTerm(docP, &nodeP->term);
  }
  else if (nodeP->type == LdQAndNode)
  {
    bson_t andArray;
    bson_append_array_begin(docP, "$and", 4, &andArray);

    for (int i = 0; i < nodeP->group.count; i++)
    {
      char key[16];
      int  keyLen = snprintf(key, sizeof(key), "%d", i);

      bson_t childDoc;
      bson_append_document_begin(&andArray, key, keyLen, &childDoc);
      bsonAppendQNode(&childDoc, nodeP->group.childV[i]);
      bson_append_document_end(&andArray, &childDoc);
    }

    bson_append_array_end(docP, &andArray);
  }
  else if (nodeP->type == LdQOrNode)
  {
    bson_t orArray;
    bson_append_array_begin(docP, "$or", 3, &orArray);

    for (int i = 0; i < nodeP->group.count; i++)
    {
      char key[16];
      int  keyLen = snprintf(key, sizeof(key), "%d", i);

      bson_t childDoc;
      bson_append_document_begin(&orArray, key, keyLen, &childDoc);
      bsonAppendQNode(&childDoc, nodeP->group.childV[i]);
      bson_append_document_end(&orArray, &childDoc);
    }

    bson_append_array_end(docP, &orArray);
  }
}



// -----------------------------------------------------------------------------
//
// bsonAppendQFilter - add Q expression filter to a BSON document
//
static void bsonAppendQFilter(bson_t* filterP, LdQNode* qExpr)
{
  bsonAppendQNode(filterP, qExpr);
}



// -----------------------------------------------------------------------------
//
// bsonAppendCoordinates - recursively append a JSON coordinate array to BSON
//
static const char* bsonAppendCoordValue(bson_t* arrayP, int* indexP, const char* p);

static const char* bsonAppendCoordArray(bson_t* parentP, const char* key, int keyLen, const char* p)
{
  bson_t array;
  bson_append_array_begin(parentP, key, keyLen, &array);

  int index = 0;

  // Skip '['
  p++;

  while (*p != 0 && *p != ']')
  {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
      p++;

    if (*p == ']')
      break;

    p = bsonAppendCoordValue(&array, &index, p);

    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
      p++;

    if (*p == ',')
      p++;
  }

  if (*p == ']')
    p++;

  bson_append_array_end(parentP, &array);
  return p;
}

static const char* bsonAppendCoordValue(bson_t* arrayP, int* indexP, const char* p)
{
  char key[16];
  int  keyLen = snprintf(key, sizeof(key), "%d", *indexP);
  (*indexP)++;

  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
    p++;

  if (*p == '[')
  {
    // Nested array
    return bsonAppendCoordArray(arrayP, key, keyLen, p);
  }
  else
  {
    // Number
    char* end;
    double val = strtod(p, &end);
    bson_append_double(arrayP, key, keyLen, val);
    return end;
  }
}



// -----------------------------------------------------------------------------
//
// bsonAppendGeoFilter - build BSON geo-query filter from DbQueryFilter geo fields
//
static void bsonAppendGeoFilter(bson_t* filterP, DbQueryFilter* f)
{
  if (f->geoRel == NULL || f->geometry == NULL || f->coordinates == NULL)
    return;

  char fieldPath[1024];
  snprintf(fieldPath, sizeof(fieldPath), "%s.@none.value", mongocEscapeDotsInKey(f->geoproperty));

  // Build reference geometry BSON
  bson_t geometry;
  bson_init(&geometry);
  bson_append_utf8(&geometry, "type", 4, f->geometry, -1);

  // Parse coordinates JSON string into BSON array
  const char* coordStr = f->coordinates;
  while (*coordStr == ' ') coordStr++;

  if (*coordStr == '[')
    bsonAppendCoordArray(&geometry, "coordinates", 11, coordStr);

  switch (f->geoRel->rel)
  {
  case LdGeoNear:
  {
    bson_t fieldDoc;
    bson_t nearDoc;
    bson_append_document_begin(filterP, fieldPath, -1, &fieldDoc);
    bson_append_document_begin(&fieldDoc, "$near", 5, &nearDoc);
    bson_append_document(&nearDoc, "$geometry", 9, &geometry);
    if (f->geoRel->maxDistance >= 0)
      bson_append_double(&nearDoc, "$maxDistance", 12, f->geoRel->maxDistance);
    if (f->geoRel->minDistance >= 0)
      bson_append_double(&nearDoc, "$minDistance", 12, f->geoRel->minDistance);
    bson_append_document_end(&fieldDoc, &nearDoc);
    bson_append_document_end(filterP, &fieldDoc);
    break;
  }

  case LdGeoWithin:
  {
    bson_t fieldDoc;
    bson_t withinDoc;
    bson_append_document_begin(filterP, fieldPath, -1, &fieldDoc);
    bson_append_document_begin(&fieldDoc, "$geoWithin", 10, &withinDoc);
    bson_append_document(&withinDoc, "$geometry", 9, &geometry);
    bson_append_document_end(&fieldDoc, &withinDoc);
    bson_append_document_end(filterP, &fieldDoc);
    break;
  }

  case LdGeoIntersects:
  case LdGeoOverlaps:
  {
    bson_t fieldDoc;
    bson_t intersectsDoc;
    bson_append_document_begin(filterP, fieldPath, -1, &fieldDoc);
    bson_append_document_begin(&fieldDoc, "$geoIntersects", 14, &intersectsDoc);
    bson_append_document(&intersectsDoc, "$geometry", 9, &geometry);
    bson_append_document_end(&fieldDoc, &intersectsDoc);
    bson_append_document_end(filterP, &fieldDoc);
    break;
  }

  case LdGeoDisjoint:
  {
    bson_t fieldDoc;
    bson_t notDoc;
    bson_t intersectsDoc;
    bson_append_document_begin(filterP, fieldPath, -1, &fieldDoc);
    bson_append_document_begin(&fieldDoc, "$not", 4, &notDoc);
    bson_append_document_begin(&notDoc, "$geoIntersects", 14, &intersectsDoc);
    bson_append_document(&intersectsDoc, "$geometry", 9, &geometry);
    bson_append_document_end(&notDoc, &intersectsDoc);
    bson_append_document_end(&fieldDoc, &notDoc);
    bson_append_document_end(filterP, &fieldDoc);
    break;
  }

  case LdGeoEquals:
  {
    bson_append_document(filterP, fieldPath, -1, &geometry);
    break;
  }

  case LdGeoContains:
  {
    bson_t fieldDoc;
    bson_t intersectsDoc;
    bson_append_document_begin(filterP, fieldPath, -1, &fieldDoc);
    bson_append_document_begin(&fieldDoc, "$geoIntersects", 14, &intersectsDoc);
    bson_append_document(&intersectsDoc, "$geometry", 9, &geometry);
    bson_append_document_end(&fieldDoc, &intersectsDoc);
    bson_append_document_end(filterP, &fieldDoc);
    break;
  }

  case LdGeoNone:
    break;
  }

  bson_destroy(&geometry);
}



// -----------------------------------------------------------------------------
//
// mongocEntityQuery -
//
int mongocEntityQuery(Tenant* tenantP, DbQueryFilter* filterP, KjNode** arrayPP)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  //
  // Build BSON filter from DbQueryFilter
  //
  bson_t filter;
  bson_init(&filter);

  if (filterP != NULL)
  {
    if (filterP->idV != NULL)
      bsonInArray(&filter, "_id", filterP->idV);

    if (filterP->idPattern != NULL)
    {
      bson_t regexDoc;
      bson_append_document_begin(&filter, "_id", 3, &regexDoc);
      bson_append_regex(&regexDoc, "$regex", 6, filterP->idPattern, "");
      bson_append_document_end(&filter, &regexDoc);
    }

    if (filterP->typeExpr != NULL && !filterP->typeExpr->isSimple)
    {
      bson_t orArray;
      bson_append_array_begin(&filter, "$or", 3, &orArray);

      for (int gix = 0; gix < filterP->typeExpr->groupCount; gix++)
      {
        char key[16];
        int  keyLen = snprintf(key, sizeof(key), "%d", gix);

        LdTypeGroup* grp = &filterP->typeExpr->groupV[gix];
        bson_t       orElem;
        bson_append_document_begin(&orArray, key, keyLen, &orElem);

        if (grp->count == 1)
        {
          bson_append_utf8(&orElem, "type", 4, grp->typeV[0], -1);
        }
        else
        {
          bson_t allDoc;
          bson_t allArray;

          bson_append_document_begin(&orElem, "type", 4, &allDoc);
          bson_append_array_begin(&allDoc, "$all", 4, &allArray);

          for (int tix = 0; tix < grp->count; tix++)
          {
            char tkey[16];
            int  tkeyLen = snprintf(tkey, sizeof(tkey), "%d", tix);
            bson_append_utf8(&allArray, tkey, tkeyLen, grp->typeV[tix], -1);
          }

          bson_append_array_end(&allDoc, &allArray);
          bson_append_document_end(&orElem, &allDoc);
        }

        bson_append_document_end(&orArray, &orElem);
      }

      bson_append_array_end(&filter, &orArray);
    }
    else if (filterP->typeV != NULL)
      bsonInArray(&filter, "type", filterP->typeV);

    if (filterP->scopeExpr != NULL)
      bsonAppendScopeFilter(&filter, filterP->scopeExpr);

    if (filterP->qExpr != NULL)
      bsonAppendQFilter(&filter, filterP->qExpr);

    if (filterP->geoRel != NULL)
      bsonAppendGeoFilter(&filter, filterP);
  }

  //
  // Build query options (limit + skip)
  //
  bson_t opts;
  bson_init(&opts);

  //
  // limit=0 means "count only, no results"
  //
  bool countOnly = (filterP != NULL && filterP->limit == 0 && filterP->count);

  if (filterP != NULL && filterP->limit > 0)
    BSON_APPEND_INT64(&opts, "limit", filterP->limit);

  if (filterP != NULL && filterP->offset > 0)
    BSON_APPEND_INT64(&opts, "skip", filterP->offset);

  //
  // Sort by createdAt (ascending) for deterministic ordering
  //
  bson_t sort;
  bson_init(&sort);
  BSON_APPEND_INT32(&sort, mongocEscapeDotsInKey(LD_VOCAB_CREATED_AT), 1);
  BSON_APPEND_DOCUMENT(&opts, "sort", &sort);
  bson_destroy(&sort);

  //
  // Count total matching documents (before limit/offset) if requested
  //
  if (filterP != NULL && filterP->count)
  {
    bson_error_t countError;
    int64_t      total = mongoc_collection_count_documents(collP, &filter, NULL, NULL, NULL, &countError);

    if (total < 0)
    {
      KT_E("mongoc: count_documents failed: %s", countError.message);
      filterP->totalCount = 0;
    }
    else
    {
      filterP->totalCount = total;
    }
  }

  //
  // For count-only mode, skip the query entirely and return an empty array
  //
  if (countOnly)
  {
    *arrayPP = kjArray(swRest.kjsonP, NULL);
    bson_destroy(&opts);
    bson_destroy(&filter);
    mongoc_collection_destroy(collP);
    mongoc_client_pool_push(poolP, clientP);
    return DB_OK;
  }

  mongoc_cursor_t* cursorP = mongoc_collection_find_with_opts(collP, &filter, &opts, NULL);

  //
  // Build result array
  //
  KjNode*       arrayP = kjArray(swRest.kjsonP, NULL);
  const bson_t* doc;

  while (mongoc_cursor_next(cursorP, &doc))
  {
    KjNode* entityP = mongocBsonToKjTree(&swRest.kalloc, doc);
    kjChildAdd(arrayP, entityP);
  }

  //
  // Check for cursor error
  //
  bson_error_t error;
  int          result = DB_OK;

  if (mongoc_cursor_error(cursorP, &error))
  {
    KT_E("mongoc: entityQuery failed: %s", error.message);
    result = DB_ERR;
  }
  else
  {
    *arrayPP = arrayP;
  }

  bson_destroy(&opts);
  bson_destroy(&filter);
  mongoc_cursor_destroy(cursorP);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}

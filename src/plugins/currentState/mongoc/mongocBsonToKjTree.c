//
// FILE            mongocBsonToKjTree.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <string.h>                                  // strcmp, strlen

#include <bson/bson.h>                               // bson_t, bson_iter_t

#include "kalloc/KAlloc.h"                           // KAlloc
#include "kalloc/kaStrdup.h"                         // kaStrdup
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjString, kjInteger, kjFloat, kjBoolean, kjNull, kjObject, kjArray, kjChildAdd
#include "kjson/kjBufferCreate.h"                    // kjBufferCreate

#include "currentState/mongoc/mongocDotEscape.h"                  // mongocUnescapeDotsInKey
#include "currentState/mongoc/mongocBsonToKjTree.h"               // Own interface



// -----------------------------------------------------------------------------
//
// bsonIterToKjNode - recursive helper to convert a bson_iter_t to KjNode children
//
static void bsonIterToKjNode(Kjson* kjsonP, KAlloc* kaP, bson_iter_t* iterP, KjNode* containerP)
{
  while (bson_iter_next(iterP))
  {
    bson_type_t    btype = bson_iter_type(iterP);
    KjNode*        nodeP = NULL;

    // Array elements have no names (BSON uses "0", "1", ... as keys - discard them)
    const char* key = NULL;

    if (containerP->type != KjArray)
    {
      key = mongocUnescapeDotsInKey(kaP, bson_iter_key(iterP));

      // Rename _id back to id
      if (strcmp(key, "_id") == 0)
        key = "id";
    }

    switch (btype)
    {
    case BSON_TYPE_UTF8:
      {
        uint32_t    len;
        const char* val = bson_iter_utf8(iterP, &len);
        nodeP = kjString(kjsonP, key, val);
      }
      break;

    case BSON_TYPE_INT32:
      nodeP = kjInteger(kjsonP, key, bson_iter_int32(iterP));
      break;

    case BSON_TYPE_INT64:
      nodeP = kjInteger(kjsonP, key, bson_iter_int64(iterP));
      break;

    case BSON_TYPE_DOUBLE:
      nodeP = kjFloat(kjsonP, key, bson_iter_double(iterP));
      break;

    case BSON_TYPE_BOOL:
      nodeP = kjBoolean(kjsonP, key, bson_iter_bool(iterP));
      break;

    case BSON_TYPE_NULL:
      nodeP = kjNull(kjsonP, key);
      break;

    case BSON_TYPE_DOCUMENT:
      {
        bson_iter_t childIter;
        bson_iter_recurse(iterP, &childIter);
        nodeP = kjObject(kjsonP, key);
        bsonIterToKjNode(kjsonP, kaP, &childIter, nodeP);
      }
      break;

    case BSON_TYPE_ARRAY:
      {
        bson_iter_t childIter;
        bson_iter_recurse(iterP, &childIter);
        nodeP = kjArray(kjsonP, key);
        bsonIterToKjNode(kjsonP, kaP, &childIter, nodeP);
      }
      break;

    default:
      // Unsupported BSON types are skipped
      break;
    }

    if (nodeP != NULL)
      kjChildAdd(containerP, nodeP);
  }
}



// -----------------------------------------------------------------------------
//
// mongocBsonToKjTree - convert a bson_t document to a KjNode tree
//
KjNode* mongocBsonToKjTree(KAlloc* kaP, const bson_t* bsonP)
{
  //
  // Create a local Kjson backed by the KAlloc
  //
  Kjson        kjsonLocal;
  Kjson*       kjsonP = kjBufferCreate(&kjsonLocal, kaP);

  KjNode*      treeP = kjObject(kjsonP, NULL);
  bson_iter_t  iter;

  bson_iter_init(&iter, bsonP);
  bsonIterToKjNode(kjsonP, kaP, &iter, treeP);

  return treeP;
}

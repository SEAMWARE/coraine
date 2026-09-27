//
// FILE            mongocBsonToTree.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <string.h>                                  // strcmp, strlen

#include <bson/bson.h>                               // bson_t, bson_iter_t

#include "corAlloc/CorAlloc.h"                       // CorAlloc
#include "corAlloc/corAllocStrdup.h"                 // corAllocStrdup
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeBuilder.h"                  // corTreeString, corTreeInteger, corTreeFloat, corTreeBoolean, corTreeNull, corTreeObject, corTreeArray, corTreeChildAdd

#include "currentState/mongoc/mongocDotEscape.h"                  // mongocUnescapeDotsInKey
#include "currentState/mongoc/mongocBsonToTree.h"                 // Own interface



// -----------------------------------------------------------------------------
//
// bsonIterToNode - recursive helper to convert a bson_iter_t to CorNode children
//
static void bsonIterToNode(CorAlloc* allocP, CorAlloc* kaP, bson_iter_t* iterP, CorNode* containerP)
{
  while (bson_iter_next(iterP))
  {
    bson_type_t    btype = bson_iter_type(iterP);
    CorNode*       nodeP = NULL;

    // Array elements have no names (BSON uses "0", "1", ... as keys - discard them)
    const char* key = NULL;

    if (containerP->type != CorArray)
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
        nodeP = corTreeString(allocP, key, val);
      }
      break;

    case BSON_TYPE_INT32:
      nodeP = corTreeInteger(allocP, key, bson_iter_int32(iterP));
      break;

    case BSON_TYPE_INT64:
      nodeP = corTreeInteger(allocP, key, bson_iter_int64(iterP));
      break;

    case BSON_TYPE_DOUBLE:
      nodeP = corTreeFloat(allocP, key, bson_iter_double(iterP));
      break;

    case BSON_TYPE_BOOL:
      nodeP = corTreeBoolean(allocP, key, bson_iter_bool(iterP));
      break;

    case BSON_TYPE_NULL:
      nodeP = corTreeNull(allocP, key);
      break;

    case BSON_TYPE_DOCUMENT:
      {
        bson_iter_t childIter;
        bson_iter_recurse(iterP, &childIter);
        nodeP = corTreeObject(allocP, key);
        bsonIterToNode(allocP, kaP, &childIter, nodeP);
      }
      break;

    case BSON_TYPE_ARRAY:
      {
        bson_iter_t childIter;
        bson_iter_recurse(iterP, &childIter);
        nodeP = corTreeArray(allocP, key);
        bsonIterToNode(allocP, kaP, &childIter, nodeP);
      }
      break;

    default:
      // Unsupported BSON types are skipped
      break;
    }

    if (nodeP != NULL)
      corTreeChildAdd(containerP, nodeP);
  }
}



// -----------------------------------------------------------------------------
//
// mongocBsonToTree - convert a bson_t document to a CorNode tree
//
CorNode* mongocBsonToTree(CorAlloc* kaP, const bson_t* bsonP)
{
  //
  // Create a local CorJson backed by the CorAlloc
  //
  CorAlloc*     allocP = kaP;

  CorNode*     treeP = corTreeObject(allocP, NULL);
  bson_iter_t  iter;

  bson_iter_init(&iter, bsonP);
  bsonIterToNode(allocP, kaP, &iter, treeP);

  return treeP;
}

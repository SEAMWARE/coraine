//
// FILE            mongocBsonToTree.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                                 // bool
#include <stdint.h>                                  // int64_t
#include <string.h>                                  // strcmp, strlen

#include <bson/bson.h>                               // bson_t, bson_iter_t

#include "corAlloc/CorAlloc.h"                       // CorAlloc
#include "corAlloc/corAllocStrdup.h"                 // corAllocStrdup
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeBuilder.h"                  // corTreeString, corTreeInteger, corTreeFloat, corTreeBoolean, corTreeNull, corTreeObject, corTreeArray, corTreeChildAdd

#include "currentState/mongoc/mongocDotEscape.h"                  // mongocUnescapeDotsInKey
#include "currentState/mongoc/mongocTreeToBson.h"                 // mongocSysTimeName, mongocSysTimesOpaque
#include "currentState/mongoc/mongocBsonToTree.h"                 // Own interface



// -----------------------------------------------------------------------------
//
// timesBack - an object of an entity's document (an attribute instance, a sub-attribute): the times it
// inherits from the entity put back - a createdAt right before its modifiedAt, both last when neither is
// there (where corNgsild puts them). Only an object with a "type" is one (mongocTreeToBson.h).
//
static void timesBack(CorAlloc* allocP, CorNode* objP, int64_t entityCreatedAt)
{
  bool     hasType = false;
  CorNode* cP      = NULL;
  CorNode* mP      = NULL;
  CorNode* mPrevP  = NULL;
  CorNode* prevP   = NULL;

  for (CorNode* nP = objP->value.head; nP != NULL; nP = nP->next)
  {
    char t = mongocSysTimeName(nP);

    if      (t == 'c') cP = nP;
    else if (t == 'm') { mP = nP; mPrevP = prevP; }
    else if ((nP->type == CorString) && (nP->name != NULL) && (nP->name[0] == 't') && (strcmp(nP->name, "type") == 0))
      hasType = true;

    prevP = nP;
  }

  if ((hasType == false) || ((cP != NULL) && (mP != NULL)))
    return;

  if (cP == NULL)
  {
    CorNode* tP = corTreeInteger(allocP, "createdAt", entityCreatedAt);

    if (mP == NULL)
      corTreeChildAdd(objP, tP);
    else
    {
      tP->next = mP;

      if (mPrevP == NULL)
        objP->value.head = tP;
      else
        mPrevP->next = tP;
    }
  }

  if (mP == NULL)
    corTreeChildAdd(objP, corTreeInteger(allocP, "modifiedAt", entityCreatedAt));
}



// -----------------------------------------------------------------------------
//
// bsonIterToNode - recursive helper to convert a bson_iter_t to CorNode children
//
// entityCreatedAt: an entity's document - the times its objects inherit put back (0: none, any other
// document). level: the depth of containerP's members below the entity, inValue: they are inside a value.
//
static void bsonIterToNode(CorAlloc* allocP, CorAlloc* kaP, bson_iter_t* iterP, CorNode* containerP, int64_t entityCreatedAt, int level, bool inValue)
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
        bool        childInValue = inValue || ((containerP->type != CorArray) && mongocSysTimesOpaque(key));

        bson_iter_recurse(iterP, &childIter);
        nodeP = corTreeObject(allocP, key);
        bsonIterToNode(allocP, kaP, &childIter, nodeP, entityCreatedAt, level + 1, childInValue);

        if ((entityCreatedAt != 0) && (childInValue == false) && (level >= 2))
          timesBack(allocP, nodeP, entityCreatedAt);
      }
      break;

    case BSON_TYPE_ARRAY:
      {
        bson_iter_t childIter;
        bool        childInValue = inValue || ((containerP->type != CorArray) && mongocSysTimesOpaque(key));

        bson_iter_recurse(iterP, &childIter);
        nodeP = corTreeArray(allocP, key);
        bsonIterToNode(allocP, kaP, &childIter, nodeP, entityCreatedAt, level, childInValue);
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
  bsonIterToNode(allocP, kaP, &iter, treeP, 0, 1, false);

  return treeP;
}



// -----------------------------------------------------------------------------
//
// mongocEntityBsonToTree - an ENTITY's document to its tree, every time in place (mongocTreeToBson.h)
//
CorNode* mongocEntityBsonToTree(CorAlloc* kaP, const bson_t* bsonP)
{
  bson_iter_t iter;
  int64_t     createdAt = 0;

  if (bson_iter_init_find(&iter, bsonP, "createdAt") && BSON_ITER_HOLDS_INT64(&iter))
    createdAt = bson_iter_int64(&iter);

  if (createdAt == 0)                                 // no createdAt (none to inherit): read as it is
    return mongocBsonToTree(kaP, bsonP);

  CorNode* treeP = corTreeObject(kaP, NULL);

  bson_iter_init(&iter, bsonP);
  bsonIterToNode(kaP, kaP, &iter, treeP, createdAt, 1, false);

  // The entity's modifiedAt while it is its createdAt - last, after the createdAt
  bool hasModifiedAt = false;

  for (CorNode* nP = treeP->value.head; (nP != NULL) && (hasModifiedAt == false); nP = nP->next)
    hasModifiedAt = (mongocSysTimeName(nP) == 'm');

  if (hasModifiedAt == false)
    corTreeChildAdd(treeP, corTreeInteger(kaP, "modifiedAt", createdAt));

  return treeP;
}

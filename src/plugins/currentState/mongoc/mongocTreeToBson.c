//
// FILE            mongocTreeToBson.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                                 // bool
#include <stdint.h>                                  // int64_t
#include <string.h>                                  // strcmp, strlen, strchr

#include <bson/bson.h>                               // bson_t, bson_append_*

#include "corTree/CorNode.h"                         // CorNode, CorValueType

#include "currentState/mongoc/mongocDotEscape.h"                  // mongocEscapeDotsInKey
#include "corNgsild/ldTermId.h"                                   // ldTermId, CorTerm*
#include "currentState/mongoc/mongocTreeToBson.h"                 // Own interface



// -----------------------------------------------------------------------------
//
// SysTimes - the entity's createdAt, for an entity's document: below the entity, a createdAt / modifiedAt
// equal to it is left out - and the entity's modifiedAt while it is its createdAt (mongocBsonToTree puts
// them back). 0: every time written (any other document, or an entity without a createdAt).
//
// Timed objects: the entity (level 0) and the objects from level 2 on - an attribute instance below its
// dataset wrapper (level 1), its sub-attributes below it - never inside a value: a Property's value is the
// user's, a GeoJSON value has a "type" of its own.
//



// -----------------------------------------------------------------------------
//
// mongocSysTimeName - a system timestamp member: 'c' for createdAt, 'm' for modifiedAt, 0 for any other
//
char mongocSysTimeName(CorNode* nodeP)
{
  if ((nodeP->type != CorInt) || (nodeP->name == NULL))
    return 0;

  if ((nodeP->name[0] == 'c') && (strcmp(nodeP->name, "createdAt") == 0))
    return 'c';

  if ((nodeP->name[0] == 'm') && (strcmp(nodeP->name, "modifiedAt") == 0))
    return 'm';

  return 0;
}



// -----------------------------------------------------------------------------
//
// mongocSysTimesOpaque - a member whose content is the user's: a value is never looked into
//
bool mongocSysTimesOpaque(const char* name)
{
  if (name == NULL)
    return false;

  switch (name[0])
  {
  case 'v': return (strcmp(name, "value") == 0) || (strcmp(name, "vocab") == 0) || (strcmp(name, "valueList") == 0);
  case 'o': return (strcmp(name, "object") == 0) || (strcmp(name, "objectList") == 0);
  case 'j': return (strcmp(name, "json") == 0);
  case 'l': return (strcmp(name, "languageMap") == 0);
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// nodeToBson - recursive helper
//
// entityCreatedAt: see SysTimes above (0: every time written). level: the node's depth below the entity
// (0: the entity), inValue: the node is inside a value.
//
static void nodeToBson(CorNode* nodeP, bson_t* bsonP, bool inArray, int arrayIndex, int64_t entityCreatedAt, int level, bool inValue)
{
  char  indexStr[16];
  const char* key;

  if (inArray)
  {
    snprintf(indexStr, sizeof(indexStr), "%d", arrayIndex);
    key = indexStr;
  }
  else
    key = mongocEscapeDotsInKey(nodeP->name);

  switch (nodeP->type)
  {
  case CorString:
    bson_append_utf8(bsonP, key, -1, nodeP->value.s, -1);
    break;

  case CorInt:
    bson_append_int64(bsonP, key, -1, nodeP->value.i);
    break;

  case CorFloat:
    bson_append_double(bsonP, key, -1, nodeP->value.f);
    break;

  case CorBoolean:
    bson_append_bool(bsonP, key, -1, nodeP->value.b);
    break;

  case CorNull:
    bson_append_null(bsonP, key, -1);
    break;

  case CorObject:
    {
      bson_t child;
      bson_append_document_begin(bsonP, key, -1, &child);

      bool timed = (entityCreatedAt != 0) && (inValue == false) && (level >= 2);
      int  ix    = 0;

      for (CorNode* childP = nodeP->value.head; childP != NULL; childP = childP->next)
      {
        if (timed && (childP->value.i == entityCreatedAt) && (mongocSysTimeName(childP) != 0))
          continue;                                   // inherited: the entity's createdAt

        nodeToBson(childP, &child, false, ix++, entityCreatedAt, level + 1, inValue || mongocSysTimesOpaque(childP->name));
      }

      bson_append_document_end(bsonP, &child);
    }
    break;

  case CorArray:
    {
      bson_t child;
      bson_append_array_begin(bsonP, key, -1, &child);

      int ix = 0;
      for (CorNode* childP = nodeP->value.head; childP != NULL; childP = childP->next)
        nodeToBson(childP, &child, true, ix++, entityCreatedAt, level, inValue);

      bson_append_array_end(bsonP, &child);
    }
    break;

  default:
    break;
  }
}



// -----------------------------------------------------------------------------
//
// treeToBson - a document's members: "id" written as "_id"
//
static void treeToBson(CorNode* treeP, bson_t* bsonP, int64_t entityCreatedAt)
{
  bson_init(bsonP);

  // treeP is a CorObject — iterate its children, appending to the bson
  // doc with "id" rewritten to "_id". The node is mutated temporarily
  // so nodeToBson sees the right key, then restored — the tree is
  // shared with the service routine / notifier, which expect "id".
  for (CorNode* childP = treeP->value.head; childP != NULL; childP = childP->next)
  {
    // The entity's modifiedAt while it is its createdAt - left out (SysTimes)
    if ((entityCreatedAt != 0) && (childP->value.i == entityCreatedAt) && (mongocSysTimeName(childP) == 'm'))
      continue;

    bool idRewritten = false;
    if (childP->name != NULL && ldTermId(childP) == CorTermId)
    {
      ldNodeRename(childP, "_id");
      idRewritten  = true;
    }
    nodeToBson(childP, bsonP, false, 0, entityCreatedAt, 1, mongocSysTimesOpaque(childP->name));
    if (idRewritten)
      ldNodeRename(childP, "id");
  }
}



// -----------------------------------------------------------------------------
//
// mongocTreeToBson - convert a CorNode (object) to a bson_t document
//
void mongocTreeToBson(CorNode* treeP, bson_t* bsonP)
{
  treeToBson(treeP, bsonP, 0);
}



// -----------------------------------------------------------------------------
//
// mongocEntityCreatedAt - an entity's createdAt member (0: none)
//
int64_t mongocEntityCreatedAt(CorNode* entityP)
{
  for (CorNode* childP = (entityP != NULL)? entityP->value.head : NULL; childP != NULL; childP = childP->next)
  {
    if (mongocSysTimeName(childP) == 'c')
      return childP->value.i;
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// mongocEntityToBson - an ENTITY to its document, the times it inherits left out (SysTimes)
//
void mongocEntityToBson(CorNode* entityP, bson_t* bsonP)
{
  treeToBson(entityP, bsonP, mongocEntityCreatedAt(entityP));
}



// -----------------------------------------------------------------------------
//
// mongocNodeAppend -
//
// The node's own ->name is ignored: the caller passes the desired bson key.
// The key is dot-escaped so that attribute IRIs with literal '.' survive the
// round-trip. Used for Merge Entity's surgical $set/$unset updates.
//
void mongocNodeAppend(bson_t* parentP, const char* key, CorNode* nodeP)
{
  char* origName = nodeP->name;
  ldNodeRename(nodeP, (char*) key);
  nodeToBson(nodeP, parentP, false, 0, 0, 0, false);
  ldNodeRename(nodeP, origName);
}



// -----------------------------------------------------------------------------
//
// mongocAttrAppend - an attribute (its dataset wrapper) under 'key' in an entity's document, the times it
// inherits from the entity (entityCreatedAt - 0: none) left out (SysTimes)
//
void mongocAttrAppend(bson_t* parentP, const char* key, CorNode* attrP, int64_t entityCreatedAt)
{
  char* origName = attrP->name;
  ldNodeRename(attrP, (char*) key);
  nodeToBson(attrP, parentP, false, 0, entityCreatedAt, 1, false);
  ldNodeRename(attrP, origName);
}

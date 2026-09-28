//
// FILE            mongocTreeToBson.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <string.h>                                  // strcmp, strlen, strchr

#include <bson/bson.h>                               // bson_t, bson_append_*

#include "corTree/CorNode.h"                         // CorNode, CorValueType

#include "currentState/mongoc/mongocDotEscape.h"                  // mongocEscapeDotsInKey
#include "currentState/mongoc/mongocTreeToBson.h"                 // Own interface
#include "corNgsild/ldTermId.h"                         // ldTermId, CorTerm*



// -----------------------------------------------------------------------------
//
// nodeToBson - recursive helper
//
static void nodeToBson(CorNode* nodeP, bson_t* bsonP, bool inArray, int arrayIndex)
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

      int ix = 0;
      for (CorNode* childP = nodeP->value.head; childP != NULL; childP = childP->next)
        nodeToBson(childP, &child, false, ix++);

      bson_append_document_end(bsonP, &child);
    }
    break;

  case CorArray:
    {
      bson_t child;
      bson_append_array_begin(bsonP, key, -1, &child);

      int ix = 0;
      for (CorNode* childP = nodeP->value.head; childP != NULL; childP = childP->next)
        nodeToBson(childP, &child, true, ix++);

      bson_append_array_end(bsonP, &child);
    }
    break;

  default:
    break;
  }
}



// -----------------------------------------------------------------------------
//
// mongocTreeToBson - convert a CorNode (object) to a bson_t document
//
void mongocTreeToBson(CorNode* treeP, bson_t* bsonP)
{
  bson_init(bsonP);

  // treeP is a CorObject — iterate its children, appending to the bson
  // doc with "id" rewritten to "_id". The node is mutated temporarily
  // so nodeToBson sees the right key, then restored — the tree is
  // shared with the service routine / notifier, which expect "id".
  for (CorNode* childP = treeP->value.head; childP != NULL; childP = childP->next)
  {
    bool idRewritten = false;
    if (childP->name != NULL && ldTermId(childP) == CorTermId)
    {
      childP->name = "_id";
      idRewritten  = true;
    }
    nodeToBson(childP, bsonP, false, 0);
    if (idRewritten)
      childP->name = "id";
  }
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
  nodeP->name = (char*) key;
  nodeToBson(nodeP, parentP, false, 0);
  nodeP->name = origName;
}

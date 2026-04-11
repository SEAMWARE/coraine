//
// FILE            mongocKjTreeToBson.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <string.h>                                  // strcmp, strlen, strchr

#include <bson/bson.h>                               // bson_t, bson_append_*

#include "kjson/KjNode.h"                            // KjNode, KjValueType

#include "currentState/mongoc/mongocDotEscape.h"                  // mongocEscapeDotsInKey
#include "currentState/mongoc/mongocKjTreeToBson.h"               // Own interface



// -----------------------------------------------------------------------------
//
// kjNodeToBson - recursive helper
//
static void kjNodeToBson(KjNode* nodeP, bson_t* bsonP, bool inArray, int arrayIndex)
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
  case KjString:
    bson_append_utf8(bsonP, key, -1, nodeP->value.s, -1);
    break;

  case KjInt:
    bson_append_int64(bsonP, key, -1, nodeP->value.i);
    break;

  case KjFloat:
    bson_append_double(bsonP, key, -1, nodeP->value.f);
    break;

  case KjBoolean:
    bson_append_bool(bsonP, key, -1, nodeP->value.b);
    break;

  case KjNull:
    bson_append_null(bsonP, key, -1);
    break;

  case KjObject:
    {
      bson_t child;
      bson_append_document_begin(bsonP, key, -1, &child);

      int ix = 0;
      for (KjNode* childP = nodeP->value.firstChildP; childP != NULL; childP = childP->next)
        kjNodeToBson(childP, &child, false, ix++);

      bson_append_document_end(bsonP, &child);
    }
    break;

  case KjArray:
    {
      bson_t child;
      bson_append_array_begin(bsonP, key, -1, &child);

      int ix = 0;
      for (KjNode* childP = nodeP->value.firstChildP; childP != NULL; childP = childP->next)
        kjNodeToBson(childP, &child, true, ix++);

      bson_append_array_end(bsonP, &child);
    }
    break;

  default:
    break;
  }
}



// -----------------------------------------------------------------------------
//
// mongocKjTreeToBson - convert a KjNode (object) to a bson_t document
//
void mongocKjTreeToBson(KjNode* treeP, bson_t* bsonP)
{
  bson_init(bsonP);

  // treeP is a KjObject - iterate its children, rename "id" to "_id"
  for (KjNode* childP = treeP->value.firstChildP; childP != NULL; childP = childP->next)
  {
    if (childP->name != NULL && strcmp(childP->name, "id") == 0)
      childP->name = "_id";
    kjNodeToBson(childP, bsonP, false, 0);
  }
}



// -----------------------------------------------------------------------------
//
// mongocKjNodeAppend -
//
// The node's own ->name is ignored: the caller passes the desired bson key.
// The key is dot-escaped so that attribute IRIs with literal '.' survive the
// round-trip. Used for Merge Entity's surgical $set/$unset updates.
//
void mongocKjNodeAppend(bson_t* parentP, const char* key, KjNode* nodeP)
{
  char* origName = nodeP->name;
  nodeP->name = (char*) key;
  kjNodeToBson(nodeP, parentP, false, 0);
  nodeP->name = origName;
}

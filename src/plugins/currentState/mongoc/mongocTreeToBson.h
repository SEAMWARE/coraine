#ifndef MONGOC_MONGOCTREETOBSON_H_
#define MONGOC_MONGOCTREETOBSON_H_

//
// FILE            mongocTreeToBson.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <bson/bson.h>                               // bson_t

#include "corTree/CorNode.h"                         // CorNode



// -----------------------------------------------------------------------------
//
// mongocTreeToBson - convert a CorNode tree to a bson_t document
//
extern void mongocTreeToBson(CorNode* treeP, bson_t* bsonP);



// -----------------------------------------------------------------------------
//
// mongocNodeAppend - append a single CorNode under an explicit bson key.
//
// Used by the Merge Entity driver to build surgical $set / $unset update
// documents: for each changed attribute we append its wrapper subtree under
// the attribute's stored name, without having to round-trip through the full
// entity conversion. The key is escaped via mongocEscapeDotsInKey().
//
extern void mongocNodeAppend(bson_t* parentP, const char* key, CorNode* nodeP);

#endif  // MONGOC_MONGOCTREETOBSON_H_

#ifndef MONGOC_MONGOCKJTREETOBSON_H_
#define MONGOC_MONGOCKJTREETOBSON_H_

//
// FILE            mongocKjTreeToBson.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <bson/bson.h>                               // bson_t

#include "kjson/KjNode.h"                            // KjNode



// -----------------------------------------------------------------------------
//
// mongocKjTreeToBson - convert a KjNode tree to a bson_t document
//
extern void mongocKjTreeToBson(KjNode* treeP, bson_t* bsonP);



// -----------------------------------------------------------------------------
//
// mongocKjNodeAppend - append a single KjNode under an explicit bson key.
//
// Used by the Merge Entity driver to build surgical $set / $unset update
// documents: for each changed attribute we append its wrapper subtree under
// the attribute's stored name, without having to round-trip through the full
// entity conversion. The key is escaped via mongocEscapeDotsInKey().
//
extern void mongocKjNodeAppend(bson_t* parentP, const char* key, KjNode* nodeP);

#endif  // MONGOC_MONGOCKJTREETOBSON_H_

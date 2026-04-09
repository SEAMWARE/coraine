#ifndef MONGOC_MONGOCBSONTOKJTREE_H_
#define MONGOC_MONGOCBSONTOKJTREE_H_

//
// FILE            mongocBsonToKjTree.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <bson/bson.h>                               // bson_t

#include "kalloc/KAlloc.h"                           // KAlloc
#include "kjson/KjNode.h"                            // KjNode



// -----------------------------------------------------------------------------
//
// mongocBsonToKjTree - convert a bson_t document to a KjNode tree
//
extern KjNode* mongocBsonToKjTree(KAlloc* kaP, const bson_t* bsonP);

#endif  // MONGOC_MONGOCBSONTOKJTREE_H_

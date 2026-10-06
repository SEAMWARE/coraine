#ifndef MONGOC_MONGOCBSONTOTREE_H_
#define MONGOC_MONGOCBSONTOTREE_H_

//
// FILE            mongocBsonToTree.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <bson/bson.h>                               // bson_t

#include "corAlloc/CorAlloc.h"                       // CorAlloc
#include "corTree/CorNode.h"                         // CorNode



// -----------------------------------------------------------------------------
//
// mongocBsonToTree - convert a bson_t document to a CorNode tree
//
extern CorNode* mongocBsonToTree(CorAlloc* kaP, const bson_t* bsonP);



// -----------------------------------------------------------------------------
//
// mongocEntityBsonToTree - an ENTITY's document to its tree: the times its objects inherit from the
// entity's createdAt put back (System timestamps, mongocTreeToBson.h). A document with every time in
// place is read as it is.
//
extern CorNode* mongocEntityBsonToTree(CorAlloc* kaP, const bson_t* bsonP);

#endif  // MONGOC_MONGOCBSONTOTREE_H_

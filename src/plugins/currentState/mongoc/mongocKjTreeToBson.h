#ifndef MONGOC_MONGOCKJTREETOBSON_H_
#define MONGOC_MONGOCKJTREETOBSON_H_

//
// FILE            mongocKjTreeToBson.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <bson/bson.h>                               // bson_t

#include "kjson/KjNode.h"                            // KjNode



// -----------------------------------------------------------------------------
//
// mongocKjTreeToBson - convert a KjNode tree to a bson_t document
//
extern void mongocKjTreeToBson(KjNode* treeP, bson_t* bsonP);

#endif  // MONGOC_MONGOCKJTREETOBSON_H_

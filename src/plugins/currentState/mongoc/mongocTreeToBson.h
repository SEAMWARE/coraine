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

#include <stdbool.h>                                 // bool
#include <stdint.h>                                  // int64_t

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




// -----------------------------------------------------------------------------
//
// System timestamps - one per created entity (coraine doc/cor-protocol-details.md § 4.10a)
//
// corNgsild gives the entity, every attribute instance and every sub-attribute a createdAt and a
// modifiedAt. An entity is created whole, with one time: its document keeps that one time - the entity's
// createdAt - and, below the entity, only the times that differ from it. A time that is not there is the
// entity's createdAt: mongocEntityBsonToTree puts it back. Never inside a value.
//
//   mongocEntityToBson      an entity's document (create, replace)
//   mongocAttrAppend        one attribute in a $set (merge, attrs set)
//   mongocEntityCreatedAt   the entity's createdAt, for mongocAttrAppend
//
extern void    mongocEntityToBson(CorNode* entityP, bson_t* bsonP);
extern void    mongocAttrAppend(bson_t* parentP, const char* key, CorNode* attrP, int64_t entityCreatedAt);
extern int64_t mongocEntityCreatedAt(CorNode* entityP);

// mongocSysTimeName - 'c' createdAt, 'm' modifiedAt (integers), 0 any other member
extern char    mongocSysTimeName(CorNode* nodeP);

// mongocSysTimesOpaque - a member whose content is the user's (value, object, json, languageMap, ...)
extern bool    mongocSysTimesOpaque(const char* name);

#endif  // MONGOC_MONGOCTREETOBSON_H_

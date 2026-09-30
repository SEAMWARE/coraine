#ifndef CORDB_CORDBINDEX_H_
#define CORDB_CORDBINDEX_H_

//
// FILE            corDbIndex.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                         // CorNode

#include "currentState/corDB/corDbStore.h"           // CorDbStore



// -----------------------------------------------------------------------------
//
// corDbEntityId - an entity's id, without looking it up
//
// The id is the FIRST child of every stored entity - corDbIndexAdd guarantees
// it on the way in, and nothing afterwards can move it, because attribute
// writes append and attribute deletes never touch "id". So this is a pointer
// hop where the rest of the plugin used to call corTreeLookup, which walks the
// child list comparing names.
//
// That mattered: finding one entity used to cost a corTreeLookup PER ENTITY over the
// whole store. Now it costs one hash.
//
extern const char* corDbEntityId(CorNode* entityP);



// -----------------------------------------------------------------------------
//
// ⭐ The index maps an entity's id to its PREDECESSOR in the store's list, which makes unlinking
// O(1) - see corDbIndex.c. So these three are the ONLY ways an entity enters, leaves or is swapped
// in the store's list: anything else would leave a neighbour's entry naming the wrong node.
// All three: the store's WRITE lock held.
//
// corDbIndexLink    - append the entity to the store and index it; puts its id first
// corDbIndexUnlink  - take it out of the store and the index, O(1). Not freed: the caller's, after
//                     the lock
// corDbIndexReplace - put newP where oldP is (same id, same position - creation order); oldP out,
//                     not freed
//
extern void corDbIndexLink(CorDbStore* storeP, CorNode* entityP);
extern void corDbIndexUnlink(CorDbStore* storeP, CorNode* entityP);
extern void corDbIndexReplace(CorDbStore* storeP, CorNode* oldP, CorNode* newP);



// -----------------------------------------------------------------------------
//
// corDbIndexLookup - the entity with this id, or NULL
//
// Call with the store lock held (read or write). O(1).
//
extern CorNode* corDbIndexLookup(CorDbStore* storeP, const char* entityId);

#endif  // CORDB_CORDBINDEX_H_

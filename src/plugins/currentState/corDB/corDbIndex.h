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
#include "kjson/KjNode.h"                            // KjNode

#include "currentState/corDB/corDbStore.h"           // CorDbStore



// -----------------------------------------------------------------------------
//
// corDbEntityId - an entity's id, without looking it up
//
// The id is the FIRST child of every stored entity - corDbIndexAdd guarantees
// it on the way in, and nothing afterwards can move it, because attribute
// writes append and attribute deletes never touch "id". So this is a pointer
// hop where the rest of the plugin used to call kjLookup, which walks the
// child list comparing names.
//
// That mattered: finding one entity used to cost a kjLookup PER ENTITY over the
// whole store. Now it costs one hash.
//
extern const char* corDbEntityId(KjNode* entityP);



// -----------------------------------------------------------------------------
//
// corDbIndexAdd - index an entity, and put its id first
//
// Call with the store's WRITE lock held. Establishes the id-first invariant
// that corDbEntityId depends on.
//
extern void corDbIndexAdd(CorDbStore* storeP, KjNode* entityP);



// -----------------------------------------------------------------------------
//
// corDbIndexRemove - drop an entity from the index
//
// Call with the store's WRITE lock held, BEFORE the entity is freed - the
// index's compare function reads the entity to get its id.
//
extern void corDbIndexRemove(CorDbStore* storeP, KjNode* entityP);



// -----------------------------------------------------------------------------
//
// corDbIndexLookup - the entity with this id, or NULL
//
// Call with the store lock held (read or write). O(1).
//
extern KjNode* corDbIndexLookup(CorDbStore* storeP, const char* entityId);

#endif  // CORDB_CORDBINDEX_H_

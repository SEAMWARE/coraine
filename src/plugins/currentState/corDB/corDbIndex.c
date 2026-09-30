//
// FILE            corDbIndex.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                 // bool
#include <stddef.h>                                  // NULL
#include <stdlib.h>                                  // abort
#include <string.h>                                  // strcmp

#include "corHash/corHash.h"                         // corHashTableCreate, corHashItemAdd, ...
#include "corTree/CorNode.h"                         // CorNode
#include "corLog/corLog.h"                           // COR_E
#include "corTree/corTreeBuilder.h"                  // corTreeChildAdd, corTreeChildRemove
#include "corTree/corTreeChildReplace.h"             // corTreeChildReplace
#include "corTree/corTreeLookup.h"                   // corTreeLookup

#include "corNgsild/ldTermId.h"                      // ldTermId, CorTerm*
#include "currentState/corDB/corDbIndex.h"           // Own interface



// -----------------------------------------------------------------------------
//
// COR_DB_INDEX_SLOTS - starting size, and the growth factor
//
// corHash never rehashes, so a table sized for a thousand entities becomes a
// thousand linked lists at a million. corDbIndexAdd grows it instead: when the
// entity count passes COR_DB_INDEX_LOAD per slot, the table is rebuilt eight
// times larger. Rebuilding is O(n) and happens log8(n) times, so the amortised
// cost is nothing and the bucket walk stays short at any size.
//
#define COR_DB_INDEX_SLOTS  1024
#define COR_DB_INDEX_LOAD      4
#define COR_DB_INDEX_GROWTH    8



// -----------------------------------------------------------------------------
//
// corDbEntityId -
//
const char* corDbEntityId(CorNode* entityP)
{
  if (entityP == NULL)
    return NULL;

  CorNode* idP = entityP->value.head;

  //
  // The invariant says this IS "id". The check is cheap and the alternative -
  // indexing an entity under whatever its first member happened to be - is a
  // store that silently cannot find its own entities.
  //
  if ((idP == NULL) || (idP->name == NULL) || (ldTermId(idP) != CorTermId))
    idP = corTreeLookup(entityP, "id");

  return ((idP != NULL) && (idP->type == CorString)) ? idP->value.s : NULL;
}



// -----------------------------------------------------------------------------
//
// idHash - djb2 over the entity id
//
static unsigned int idHash(const char* name)
{
  unsigned int hash = 5381;

  while (*name != '\0')
  {
    hash = ((hash << 5) + hash) + (unsigned char) *name;
    name++;
  }

  return hash;
}



// -----------------------------------------------------------------------------
//
// ⭐⭐ THE INDEX MAPS AN ENTITY ID TO THE ENTITY'S PREDECESSOR - NOT TO THE ENTITY ⭐⭐
//
// The store's entities are a singly linked list (a CorNode array: head, tail, 'next'), and unlinking
// one needs the node BEFORE it. The index used to hand back the entity itself, so a delete found
// it in O(1) and then walked the list from the head to find its predecessor (corTreeChildRemove),
// holding the tenant's write lock: 83k deletes/s with 5 000 entities in the store, 35k with 40 000
// (2026-09-30). Replace and bulk update did the same through corTreeChildReplace.
//
// So the value stored for an id is `prevEntityP`, the node before the entity:
//
//   - the FIRST entity's predecessor is the "entities" array node itself (storeP->entities) - a
//     sentinel that is never freed, so no entry is ever NULL, and entityAfter() tells the two
//     apart by type: the array's successor is its head, an entity's is its 'next';
//   - lookup is one hop further than before: entityAfter(prevEntityP);
//   - unlink is O(1): prevEntityP->next = entity->next - and the SUCCESSOR's entry, which named the
//     unlinked entity as its predecessor, now names prevEntityP;
//   - a node swap (replace, bulk update) changes nothing in the swapped entity's own entry - its
//     predecessor is the same - but the successor's entry named the OLD node, and must name the new.
//
// ⚠️ So nothing outside this file may link, unlink or swap a store entity: corDbIndexLink,
// corDbIndexUnlink and corDbIndexReplace are the only ways, and nobody outside sees a predecessor -
// corDbIndexLookup returns the entity. An entry left naming a freed node is a use-after-free on
// the next lookup of the neighbour's id; a debug build checks the neighbour after every change
// (indexCheck).
//
// 0 extra bytes: the entry held a pointer before and holds a pointer now. A 'prev' in CorNode would
// have cost 8 bytes on EVERY node of every tree, stored or in a request - ~200 bytes per stored
// entity of the perf fixture's size.
//
static inline CorNode* entityAfter(CorNode* prevEntityP)
{
  return (prevEntityP->type == CorArray) ? prevEntityP->value.head : prevEntityP->next;
}



// -----------------------------------------------------------------------------
//
// idCompare - is the entity after this predecessor the one with this id?
//
// corHash does not keep the key: it hands the lookup name and the stored DATA to this function,
// and the data is the entity's predecessor, whose successor carries the id. So there is nothing to
// copy and nothing whose lifetime has to be managed alongside the entity's - but the LIST must be
// consistent with the table whenever this runs: every change below orders its steps for that.
//
static int idCompare(const char* name, void* itemP)
{
  CorNode*    entityP = entityAfter((CorNode*) itemP);
  const char* id      = corDbEntityId(entityP);

  return (id != NULL) ? strcmp(name, id) : 1;
}



// -----------------------------------------------------------------------------
//
// idFirst - make "id" the first child, so corDbEntityId is a pointer hop
//
// Done ONCE, on the way in. Attribute writes append and attribute deletes never
// touch "id", so nothing afterwards can break it.
//
static void idFirst(CorNode* entityP)
{
  CorNode* first = entityP->value.head;

  if ((first != NULL) && (first->name != NULL) && (ldTermId(first) == CorTermId))
    return;

  CorNode* prev = NULL;

  for (CorNode* p = first; p != NULL; prev = p, p = p->next)
  {
    if ((p->name == NULL) || (ldTermId(p) != CorTermId))
      continue;

    if (prev != NULL)
      prev->next = p->next;

    if (entityP->value.tail == p)
      entityP->value.tail = prev;

    p->next = entityP->value.head;
    entityP->value.head = p;
    return;
  }
}



// -----------------------------------------------------------------------------
//
// indexRebuild - a bigger table, with everything in it
//
static void indexRebuild(CorDbStore* storeP, int slots)
{
  CorHashTable* newP = corHashTableCreate(NULL, idHash, idCompare, slots);

  if (newP == NULL)
    return;                                          // keep the old one; slower, not wrong

  CorNode* prevEntityP = storeP->entities;          // the first entity's predecessor: the array

  for (CorNode* eP = storeP->entities->value.head; eP != NULL; prevEntityP = eP, eP = eP->next)
  {
    const char* id = corDbEntityId(eP);

    if (id != NULL)
      corHashItemAdd(newP, id, prevEntityP);
  }

  if (storeP->idToPrevEntity != NULL)
    corHashRelease(storeP->idToPrevEntity);

  storeP->idToPrevEntity = newP;
  storeP->idxSlots       = slots;
}



// -----------------------------------------------------------------------------
//
// entryAdd / entryDel - one id's entry, set or dropped. Remove-then-add: corHash does not check
// for duplicates, and a second entry for an id survives the first's removal (see corDbIndexLink).
//
static void entryDel(CorDbStore* storeP, const char* id)
{
  if (corHashItemRemove(storeP->idToPrevEntity, id) == 0)
    storeP->idxCount -= 1;
}

static void entryAdd(CorDbStore* storeP, const char* id, CorNode* prevEntityP)
{
  entryDel(storeP, id);
  corHashItemAdd(storeP->idToPrevEntity, id, prevEntityP);
  storeP->idxCount += 1;
}



// -----------------------------------------------------------------------------
//
// indexCheck - DEBUG builds: does the entity after this one still look itself up correctly?
//
// The one thing a slip in the predecessor bookkeeping produces is a neighbour whose entry names a
// node that is no longer before it. This looks the neighbour up and aborts, loudly, if the answer
// is not the neighbour - at the change that broke it, not at some later request that reads freed
// memory. Release builds: nothing.
//
static void indexCheck(CorDbStore* storeP, CorNode* entityP, const char* what)
{
#ifdef DEBUG
  if (entityP == NULL)
    return;

  const char* id  = corDbEntityId(entityP);
  CorNode*    hit = (id != NULL) ? corDbIndexLookup(storeP, id) : entityP;

  if (hit != entityP)
  {
    COR_E("corDB index: after %s, '%s' looks up as %p, not %p - the predecessor index is corrupt", what, id ? id : "(no id)", (void*) hit, (void*) entityP);
    abort();
  }
#else
  (void) storeP;
  (void) entityP;
  (void) what;
#endif
}



// -----------------------------------------------------------------------------
//
// indexReady - the table exists, or it cannot (out of memory: the callers' fallback walks then work)
//
static bool indexReady(CorDbStore* storeP)
{
  if (storeP->idToPrevEntity == NULL)
    indexRebuild(storeP, COR_DB_INDEX_SLOTS);

  return storeP->idToPrevEntity != NULL;
}



// -----------------------------------------------------------------------------
//
// corDbIndexLink -
//
void corDbIndexLink(CorDbStore* storeP, CorNode* entityP)
{
  if ((storeP == NULL) || (entityP == NULL))
    return;

  idFirst(entityP);

  CorNode* entities    = storeP->entities;
  CorNode* prevEntityP = (entities->value.tail != NULL) ? entities->value.tail : entities;

  corTreeChildAdd(entities, entityP);

  //
  // The entity is in the list BEFORE it is in the table - the table is built from the list when it
  // is first needed (indexRebuild), and an entry added on top of that would be the duplicate
  // entryAdd removes. corHash does not check: a second entry for an id survives the first's
  // removal, and every later lookup returns a pointer into freed memory (it happened - the first
  // entity ever created, and one more at every growth rebuild).
  //
  if (indexReady(storeP) == false)
    return;

  const char* id = corDbEntityId(entityP);

  if (id == NULL)
    return;

  entryAdd(storeP, id, prevEntityP);

  if (storeP->idxCount > (storeP->idxSlots * COR_DB_INDEX_LOAD))
    indexRebuild(storeP, storeP->idxSlots * COR_DB_INDEX_GROWTH);

  indexCheck(storeP, entityP, "link");
}



// -----------------------------------------------------------------------------
//
// corDbIndexUnlink -
//
// The order matters: idCompare reads the LIST, so each entry is touched while the list still
// agrees with it - the unlinked entity's own entry dropped while it is still linked, the
// successor's re-pointed, and only then the list changed.
//
void corDbIndexUnlink(CorDbStore* storeP, CorNode* entityP)
{
  if ((storeP == NULL) || (entityP == NULL))
    return;

  CorNode*    entities = storeP->entities;
  const char* id       = corDbEntityId(entityP);
  CorNode*    prevEntityP = NULL;

  if ((storeP->idToPrevEntity != NULL) && (id != NULL))
    prevEntityP = (CorNode*) corHashItemLookup(storeP->idToPrevEntity, id);

  if ((prevEntityP == NULL) || (entityAfter(prevEntityP) != entityP))
  {
    //
    // Not in the table (it failed to build, or the entity has no id): the walk, as before. Still
    // correct - the successor's entry, if there is one, is re-derived by a rebuild on next growth.
    //
    corTreeChildRemove(entities, entityP);
    if (storeP->idToPrevEntity != NULL)
      indexRebuild(storeP, storeP->idxSlots);
    return;
  }

  CorNode* nextP = entityP->next;

  entryDel(storeP, id);

  if (nextP != NULL)
  {
    const char* nextId = corDbEntityId(nextP);

    if (nextId != NULL)
    {
      entryDel(storeP, nextId);                     // named entityP: drop it while that still holds
      corHashItemAdd(storeP->idToPrevEntity, nextId, prevEntityP);
      storeP->idxCount += 1;
    }
  }

  if (prevEntityP == entities)
    entities->value.head = nextP;
  else
    prevEntityP->next = nextP;

  if (entities->value.tail == entityP)
    entities->value.tail = (prevEntityP == entities) ? NULL : prevEntityP;

  entityP->next = NULL;

  indexCheck(storeP, nextP, "unlink");
}



// -----------------------------------------------------------------------------
//
// corDbIndexReplace -
//
void corDbIndexReplace(CorDbStore* storeP, CorNode* oldP, CorNode* newP)
{
  if ((storeP == NULL) || (oldP == NULL) || (newP == NULL))
    return;

  idFirst(newP);

  CorNode*    entities    = storeP->entities;
  const char* id          = corDbEntityId(oldP);
  CorNode*    prevEntityP = NULL;

  if ((storeP->idToPrevEntity != NULL) && (id != NULL))
    prevEntityP = (CorNode*) corHashItemLookup(storeP->idToPrevEntity, id);

  if ((prevEntityP == NULL) || (entityAfter(prevEntityP) != oldP))
  {
    corTreeChildReplace(entities, oldP, newP);       // not in the table: the walk, as before
    if (storeP->idToPrevEntity != NULL)
      indexRebuild(storeP, storeP->idxSlots);
    return;
  }

  CorNode* nextP = oldP->next;

  //
  // The swapped entity's own entry names its predecessor, which does not change. The successor's
  // entry names oldP - dropped while oldP is still linked (idCompare reads through it), re-added
  // naming newP once newP is in its place.
  //
  const char* nextId = (nextP != NULL) ? corDbEntityId(nextP) : NULL;

  if (nextId != NULL)
    entryDel(storeP, nextId);

  newP->next = nextP;
  oldP->next = NULL;

  if (prevEntityP == entities)
    entities->value.head = newP;
  else
    prevEntityP->next = newP;

  if (entities->value.tail == oldP)
    entities->value.tail = newP;

  if (nextId != NULL)
  {
    corHashItemAdd(storeP->idToPrevEntity, nextId, newP);
    storeP->idxCount += 1;
  }

  indexCheck(storeP, newP,  "replace");
  indexCheck(storeP, nextP, "replace");
}



// -----------------------------------------------------------------------------
//
// corDbIndexLookup -
//
CorNode* corDbIndexLookup(CorDbStore* storeP, const char* entityId)
{
  if ((storeP == NULL) || (storeP->idToPrevEntity == NULL) || (entityId == NULL))
    return NULL;

  CorNode* prevEntityP = (CorNode*) corHashItemLookup(storeP->idToPrevEntity, entityId);

  return (prevEntityP != NULL) ? entityAfter(prevEntityP) : NULL;
}

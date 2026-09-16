//
// FILE            corDbIndex.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp

#include "khash/khash.h"                             // khashTableCreate, khashItemAdd, ...
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjLookup.h"                          // kjLookup

#include "currentState/corDB/corDbIndex.h"           // Own interface



// -----------------------------------------------------------------------------
//
// COR_DB_INDEX_SLOTS - starting size, and the growth factor
//
// khash never rehashes, so a table sized for a thousand entities becomes a
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
const char* corDbEntityId(KjNode* entityP)
{
  if (entityP == NULL)
    return NULL;

  KjNode* idP = entityP->value.firstChildP;

  //
  // The invariant says this IS "id". The check is cheap and the alternative -
  // indexing an entity under whatever its first member happened to be - is a
  // store that silently cannot find its own entities.
  //
  if ((idP == NULL) || (idP->name == NULL) || (strcmp(idP->name, "id") != 0))
    idP = kjLookup(entityP, "id");

  return ((idP != NULL) && (idP->type == KjString)) ? idP->value.s : NULL;
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
// idCompare - does this stored entity have this id?
//
// khash does not keep the key: it hands the lookup name and the stored DATA to
// this function, and the data is the entity, which carries its own id. So there
// is nothing to copy and nothing whose lifetime has to be managed alongside the
// entity's.
//
static int idCompare(const char* name, void* itemP)
{
  const char* id = corDbEntityId((KjNode*) itemP);

  return (id != NULL) ? strcmp(name, id) : 1;
}



// -----------------------------------------------------------------------------
//
// idFirst - make "id" the first child, so corDbEntityId is a pointer hop
//
// Done ONCE, on the way in. Attribute writes append and attribute deletes never
// touch "id", so nothing afterwards can break it.
//
static void idFirst(KjNode* entityP)
{
  KjNode* first = entityP->value.firstChildP;

  if ((first != NULL) && (first->name != NULL) && (strcmp(first->name, "id") == 0))
    return;

  KjNode* prev = NULL;

  for (KjNode* p = first; p != NULL; prev = p, p = p->next)
  {
    if ((p->name == NULL) || (strcmp(p->name, "id") != 0))
      continue;

    if (prev != NULL)
      prev->next = p->next;

    if (entityP->lastChild == p)
      entityP->lastChild = prev;

    p->next = entityP->value.firstChildP;
    entityP->value.firstChildP = p;
    return;
  }
}



// -----------------------------------------------------------------------------
//
// indexRebuild - a bigger table, with everything in it
//
static void indexRebuild(CorDbStore* storeP, int slots)
{
  KHashTable* newP = khashTableCreate(NULL, idHash, idCompare, slots);

  if (newP == NULL)
    return;                                          // keep the old one; slower, not wrong

  KjNode* entities = kjLookup(storeP->tree, "entities");

  if (entities != NULL)
  {
    for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
    {
      const char* id = corDbEntityId(eP);

      if (id != NULL)
        khashItemAdd(newP, id, eP);
    }
  }

  if (storeP->idIndex != NULL)
    khashRelease(storeP->idIndex);

  storeP->idIndex  = newP;
  storeP->idxSlots = slots;
}



// -----------------------------------------------------------------------------
//
// corDbIndexAdd -
//
void corDbIndexAdd(CorDbStore* storeP, KjNode* entityP)
{
  if ((storeP == NULL) || (entityP == NULL))
    return;

  idFirst(entityP);

  if (storeP->idIndex == NULL)
    indexRebuild(storeP, COR_DB_INDEX_SLOTS);

  if (storeP->idIndex == NULL)
    return;

  const char* id = corDbEntityId(entityP);

  if (id == NULL)
    return;

  //
  // Remove any existing entry for this id before adding, so an id can never be
  // in the table twice.
  //
  // khashItemAdd does not check for duplicates, and a duplicate is invisible
  // until it is fatal: khashItemRemove unlinks the FIRST match and reports
  // success, so the second entry survives the delete and every later lookup
  // returns a pointer to the freed entity. That is not hypothetical - it is the
  // bug this line fixes. indexRebuild below walks the entity list, and the
  // caller has ALREADY appended the new entity to that list, so building the
  // table indexed it once and the add indexed it again. First entity ever
  // created, and one more at every growth rebuild.
  //
  // One bucket walk per add, which is the same walk the add does anyway.
  //
  if (khashItemRemove(storeP->idIndex, id) == 0)
    storeP->idxCount -= 1;

  khashItemAdd(storeP->idIndex, id, entityP);
  storeP->idxCount += 1;

  if (storeP->idxCount > (storeP->idxSlots * COR_DB_INDEX_LOAD))
    indexRebuild(storeP, storeP->idxSlots * COR_DB_INDEX_GROWTH);
}



// -----------------------------------------------------------------------------
//
// corDbIndexRemove -
//
void corDbIndexRemove(CorDbStore* storeP, KjNode* entityP)
{
  if ((storeP == NULL) || (storeP->idIndex == NULL) || (entityP == NULL))
    return;

  const char* id = corDbEntityId(entityP);

  if (id == NULL)
    return;

  if (khashItemRemove(storeP->idIndex, id) == 0)
    storeP->idxCount -= 1;
}



// -----------------------------------------------------------------------------
//
// corDbIndexLookup -
//
KjNode* corDbIndexLookup(CorDbStore* storeP, const char* entityId)
{
  if ((storeP == NULL) || (storeP->idIndex == NULL) || (entityId == NULL))
    return NULL;

  return (KjNode*) khashItemLookup(storeP->idIndex, entityId);
}

//
// FILE            dbEntityTypeV.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                   // NULL

#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corRest/CorRestState.h"                     // corRest
#include "db/DbDriver.h"                              // db, DB_OK
#include "db/dbEntityTypeV.h"                         // Own interface



// -----------------------------------------------------------------------------
//
// dbEntityTypeV - the types of the local copy of an entity, NULL-terminated, NULL when there is none
//
// A write that does not say the entity's type (PATCH/PUT/DELETE .../attrs/{attr}, POST .../attrs) takes
// it from the local copy, to know which parts of a registration cover the entity. The types alone
// (db.entityTypes) where the store has that, the entity otherwise. Allocated in corRest.kalloc.
//
char** dbEntityTypeV(Tenant* tenantP, const char* entityId)
{
  if ((tenantP == NULL) || (entityId == NULL))
    return NULL;

  CorNode* typeP = NULL;

  if (db.entityTypes != NULL)
  {
    if (db.entityTypes(tenantP, entityId, &typeP) != DB_OK)
      return NULL;
  }
  else if (db.entityRetrieve != NULL)
  {
    CorNode* entityP = NULL;
    if ((db.entityRetrieve(tenantP, entityId, &entityP) != DB_OK) || (entityP == NULL))
      return NULL;
    typeP = corTreeLookup(entityP, "type");
  }

  if (typeP == NULL)
    return NULL;

  if (typeP->type == CorString)
  {
    char** typeV = (char**) corAlloc(&corRest.kalloc, 2 * sizeof(char*));
    typeV[0] = typeP->value.s;
    typeV[1] = NULL;
    return typeV;
  }

  if (typeP->type != CorArray)
    return NULL;

  int n = 0;
  for (CorNode* itemP = typeP->value.head; itemP != NULL; itemP = itemP->next)
    n++;

  char** typeV = (char**) corAlloc(&corRest.kalloc, (n + 1) * sizeof(char*));
  int    ix    = 0;
  for (CorNode* itemP = typeP->value.head; itemP != NULL; itemP = itemP->next)
    if (itemP->type == CorString)
      typeV[ix++] = itemP->value.s;
  typeV[ix] = NULL;

  return (ix > 0) ? typeV : NULL;
}

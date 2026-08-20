//
// FILE            corDbEntityCreate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                   // strcmp

#include "ktrace/kTrace.h"                            // KT_E
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjBuilder.h"                          // kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup

#include "db/DbDriver.h"                              // DB_OK, DB_ALREADY_EXISTS, DB_ERR, DB_INVALID_GEOMETRY, Tenant
#include "shared/geoMatch.h"                          // geoEntityValidate
#include "currentState/corDB/corDbStore.h"          // corDbEntities
#include "currentState/corDB/corDbEntityCreate.h"   // Own interface



// -----------------------------------------------------------------------------
//
// corDbEntityCreate -
//
int corDbEntityCreate(Tenant* tenantP, const char* entityId, KjNode* entityP)
{
  KjNode* entities = corDbEntities(tenantP);

  //
  // Check for duplicate
  //
  for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
  {
    KjNode* idP = kjLookup(eP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, entityId) == 0)
      return DB_ALREADY_EXISTS;
  }

  //
  // Reject geometry a 2dsphere index would refuse (degenerate / self-
  // intersecting polygon). mongoc gets this from its geo index on insert;
  // the in-memory store validates via the shared GEOS engine so the broker
  // can map it to 400 BadRequestData instead of silently storing it.
  //
  if (!geoEntityValidate(entityP))
    return DB_INVALID_GEOMETRY;

  //
  // Deep-clone the entity tree (using malloc, not a buffer allocator)
  //
  KjNode* cloneP = kjClone(NULL, entityP);
  if (cloneP == NULL)
  {
    KT_E("corDB: kjClone failed for entity '%s'", entityId);
    return DB_ERR;
  }

  kjChildAdd(entities, cloneP);

  return DB_OK;
}

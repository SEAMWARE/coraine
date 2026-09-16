//
// FILE            corDbEntityCreate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                 // bool
#include <string.h>                                   // strcmp

#include "ktrace/kTrace.h"                            // KT_E
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjBuilder.h"                          // kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup

#include "db/DbDriver.h"                              // DB_OK, DB_ALREADY_EXISTS, DB_ERR, DB_INVALID_GEOMETRY, Tenant
#include "shared/geoMatch.h"                          // geoEntityValidate
#include "currentState/corDB/corDbIndex.h"        // corDbIndexAdd
#include "currentState/corDB/corDbStore.h"          // corDbEntities
#include "currentState/corDB/corDbEntityCreate.h"   // Own interface



// -----------------------------------------------------------------------------
//
// corDbEntityCreate -
//
int corDbEntityCreate(Tenant* tenantP, const char* entityId, KjNode* entityP)
{
  COR_DB_WRITE(tenantP);

  KjNode* entities = corDbEntities(tenantP);

  //
  // Check for duplicate
  //
  //
  // One hop via the id index instead of a walk of the whole store with a
  // kjLookup per entity. The loop shape is kept so the body below is unchanged:
  // indexed, it runs exactly once for the hit and not at all for a miss;
  // unindexed - a store that predates the index - it walks as it always did.
  //
  CorDbStore* idxStoreP = corDbStoreOf(tenantP);
  KjNode*     idxHitP   = corDbIndexLookup(idxStoreP, entityId);
  bool        indexed   = (idxStoreP != NULL) && (idxStoreP->idIndex != NULL);

  for (KjNode* eP = indexed ? idxHitP : entities->value.firstChildP;
       eP != NULL;
       eP = indexed ? NULL : eP->next)
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
  corDbIndexAdd(corDbStoreOf(tenantP), cloneP);

  return DB_OK;
}

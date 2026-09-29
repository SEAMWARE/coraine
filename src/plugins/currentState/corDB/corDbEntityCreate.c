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

#include "corLog/corLog.h"                            // COR_E
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeFree.h"                      // corTreeFree
#include "corTree/corTreeBuilder.h"                   // corTreeChildAdd
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "db/DbDriver.h"                              // DB_OK, DB_ALREADY_EXISTS, DB_ERR, DB_INVALID_GEOMETRY, Tenant
#include "shared/geoMatch.h"                          // geoEntityValidate
#include "currentState/corDB/corDbIndex.h"        // corDbIndexAdd
#include "currentState/corDB/corDbStore.h"          // corDbEntities
#include "currentState/corDB/corDbEntityCreate.h"   // Own interface



// -----------------------------------------------------------------------------
//
// corDbEntityCreate -
//
int corDbEntityCreate(Tenant* tenantP, const char* entityId, CorNode* entityP)
{
  //
  // The deep clone reads nothing but the request's entity, so it is done BEFORE the write lock -
  // under it, it was most of the time the lock was held, and every other writer of the tenant
  // waited it out (see corDbEntityBulkCreate). Not needed after all (the id exists, the geometry
  // is refused): freed, outside the lock.
  //
  // The geometry check stays under the lock: the GEOS context (shared/geoMatch.c) is ONE for the
  // process, and GEOS wants one per thread for concurrent use.
  //
  CorNode* cloneP = corTreeClone(NULL, entityP);   // malloc, not a buffer allocator: store lifetime
  int      rc     = DB_OK;

  {
    COR_DB_WRITE(tenantP);

    CorNode* entities = corDbEntities(tenantP);

    //
    // Check for duplicate
    //
    // One hop via the id index instead of a walk of the whole store with a
    // corTreeLookup per entity. The loop shape is kept so the body below is unchanged:
    // indexed, it runs exactly once for the hit and not at all for a miss;
    // unindexed - a store that predates the index - it walks as it always did.
    //
    CorDbStore* idxStoreP = corDbStoreOf(tenantP);
    CorNode*    idxHitP   = corDbIndexLookup(idxStoreP, entityId);
    bool        indexed   = (idxStoreP != NULL) && (idxStoreP->idIndex != NULL);

    for (CorNode* eP = indexed ? idxHitP : entities->value.head;
         eP != NULL;
         eP = indexed ? NULL : eP->next)
    {
      CorNode* idP = corTreeLookup(eP, "id");

      if (idP != NULL && idP->type == CorString && strcmp(idP->value.s, entityId) == 0)
      {
        rc = DB_ALREADY_EXISTS;
        break;
      }
    }

    if (rc == DB_OK)
    {
      //
      // Reject geometry a 2dsphere index would refuse (degenerate / self-
      // intersecting polygon). mongoc gets this from its geo index on insert;
      // the in-memory store validates via the shared GEOS engine so the broker
      // can map it to 400 BadRequestData instead of silently storing it.
      //
      if (!geoEntityValidate(entityP))
        rc = DB_INVALID_GEOMETRY;
      else if (cloneP == NULL)
      {
        COR_E("corDB: corTreeClone failed for entity '%s'", entityId);
        rc = DB_ERR;
      }
      else
      {
        corTreeChildAdd(entities, cloneP);
        corDbIndexAdd(corDbStoreOf(tenantP), cloneP);
        return DB_OK;
      }
    }
  }

  if (cloneP != NULL)   // not stored - and freed outside the lock
    corTreeFree(cloneP);

  return rc;
}

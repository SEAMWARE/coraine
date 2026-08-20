//
// FILE            corDbEntityReplace.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                    // strcmp

#include "ktrace/kTrace.h"                             // KT_E
#include "kjson/KjNode.h"                              // KjNode
#include "kjson/kjClone.h"                             // kjClone
#include "kjson/kjLookup.h"                            // kjLookup
#include "kjson/kjFree.h"                              // kjFree
#include "kjson/kjChildReplace.h"                      // kjChildReplace
#include "corRest/CorRestState.h"                        // corRest

#include "db/DbDriver.h"                               // DB_OK, DB_NOT_FOUND, DB_ERR, Tenant
#include "currentState/corDB/corDbStore.h"           // corDbEntities
#include "currentState/corDB/corDbEntityReplace.h"   // Own interface



// -----------------------------------------------------------------------------
//
// corDbEntityReplace -
//
int corDbEntityReplace(Tenant* tenantP, const char* entityId, KjNode* newEntityP, KjNode** oldEntityPP)
{
  KjNode* entities = corDbEntities(tenantP);

  for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
  {
    KjNode* idP = kjLookup(eP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, entityId) == 0)
    {
      KjNode* cloneP = kjClone(NULL, newEntityP);
      if (cloneP == NULL)
      {
        KT_E("corDB: kjClone failed for entity '%s'", entityId);
        return DB_ERR;
      }

      // Replace in place so the entity keeps its store (creation-order)
      // position — a GET without orderBy stays stable and matches mongoc,
      // which preserves createdAt on Replace.
      kjChildReplace(entities, eP, cloneP);

      // Hand the caller a request-arena copy of the pre-replace entity (freed at
      // request end, matching mongoc's oldEntityPP), then free the malloc store
      // node — returning the raw malloc node would leak (no caller frees it).
      if (oldEntityPP != NULL)
        *oldEntityPP = kjClone(corRest.kjsonP, eP);
      kjFree(eP);

      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

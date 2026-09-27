//
// FILE            corDbEntityMerge.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corDB change-set persistence for Merge Entity / Partial Attribute Update.
//
// The NGSI-LD merge itself is done by the broker against the request-arena tree
// returned by db.entityRetrieve; this file applies the resulting change report
// to the LIVE stored entity. Only the attributes the report names are touched —
// a PATCH on one attribute of a 2000-attribute entity does not re-clone the
// whole entity.
//
// The tenant store uses a malloc-backed allocator, so any node grafted into the
// live tree is cloned with the NULL (malloc) allocator; replaced/removed nodes
// are corTreeFree'd.
//

#include <stdbool.h>                                 // bool
#include <string.h>                                   // strcmp

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeFree.h"                      // corTreeFree
#include "corTree/corTreeBuilder.h"                   // corTreeChildRemove, corTreeChildAdd
#include "corTree/corTreeChildReplace.h"              // corTreeChildReplace

#include "corNgsild/LdVocab.h"                         // LD_VOCAB_MODIFIED_AT, LD_VOCAB_SCOPE
#include "corNgsild/ldEntityMerge.h"                   // LdMergeReport

#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, DB_INVALID_GEOMETRY, Tenant
#include "shared/geoMatch.h"                          // geoEntityValidate
#include "currentState/corDB/corDbIndex.h"        // corDbIndexLookup
#include "currentState/corDB/corDbStore.h"          // corDbEntities
#include "currentState/corDB/corDbEntityMerge.h"    // Own interface



// -----------------------------------------------------------------------------
//
// replaceOrAdd - graft a malloc-clone of `srcNode` into `live` under `name`,
// replacing (and freeing) any existing same-named child.
//
static void replaceOrAdd(CorNode* live, const char* name, CorNode* srcNode)
{
  if (srcNode == NULL)
    return;

  CorNode* clone = corTreeClone(NULL, srcNode); // NULL allocator == malloc == store lifetime
  CorNode* old  = corTreeLookup(live, name);

  if (old != NULL)
  {
    corTreeChildReplace(live, old, clone);
    corTreeFree(old);
  }
  else
    corTreeChildAdd(live, clone);
}



// -----------------------------------------------------------------------------
//
// corDbApplyReportToLive - apply a merge report to a live stored entity.
//
// `merged` is the already-merged request-arena tree the report was produced
// against; the new attribute wrappers (and refreshed modifiedAt/type/scope) are
// copied from it into `live`. Shared by the single-entity and batch paths.
//
void corDbApplyReportToLive(CorNode* live, CorNode* merged, LdMergeReport* reportP)
{
  bool anyChange = false;

  if (reportP != NULL && reportP->changes != NULL)
  {
    for (CorNode* change = reportP->changes->value.firstChildP; change != NULL; change = change->next)
    {
      CorNode* attrNameP = corTreeLookup(change, "attr");
      CorNode* reasonP  = corTreeLookup(change, "reason");

      if (attrNameP == NULL || reasonP == NULL || attrNameP->type != CorString || reasonP->type != CorString)
        continue;

      const char* attrName = attrNameP->value.s;
      const char* reason   = reasonP->value.s;

      if (strcmp(reason, "attributeDeleted") == 0)
      {
        CorNode* old = corTreeLookup(live, attrName);
        if (old != NULL)
        {
          corTreeChildRemove(live, old);
          corTreeFree(old);
          anyChange = true;
        }
      }
      else
      {
        replaceOrAdd(live, attrName, corTreeLookup(merged, attrName));
        anyChange = true;
      }
    }
  }

  if (anyChange)
  {
    replaceOrAdd(live, LD_VOCAB_MODIFIED_AT, corTreeLookup(merged, LD_VOCAB_MODIFIED_AT));
    replaceOrAdd(live, "type",               corTreeLookup(merged, "type"));
    replaceOrAdd(live, LD_VOCAB_SCOPE,        corTreeLookup(merged, LD_VOCAB_SCOPE));
  }
}



// -----------------------------------------------------------------------------
//
// corDbEntityChangesApply - persist a merged single entity (DB driver entry)
//
int corDbEntityChangesApply(Tenant* tenantP, const char* entityId,
                            CorNode* mergedEntity, LdMergeReport* reportP)
{
  COR_DB_WRITE(tenantP);

  // Re-validate the GeoProperty values of the COMPLETE merged entity before it
  // touches the store. A PATCH/merge fragment that omits the attribute type is
  // validated as a plain Property (geo check skipped), so a wholesale-replaced
  // GeoProperty value such as {"type":"Polygon"} (no coordinates) would slip
  // through and persist as broken geometry. Same DB_INVALID_GEOMETRY → 400
  // contract as create.
  if (!geoEntityValidate(mergedEntity))
    return DB_INVALID_GEOMETRY;

  CorNode* entities = corDbEntities(tenantP);

  //
  // One hop via the id index instead of a walk of the whole store with a
  // corTreeLookup per entity. The loop shape is kept so the body below is unchanged:
  // indexed, it runs exactly once for the hit and not at all for a miss;
  // unindexed - a store that predates the index - it walks as it always did.
  //
  CorDbStore* idxStoreP = corDbStoreOf(tenantP);
  CorNode*    idxHitP   = corDbIndexLookup(idxStoreP, entityId);
  bool        indexed   = (idxStoreP != NULL) && (idxStoreP->idIndex != NULL);

  for (CorNode* eP = indexed ? idxHitP : entities->value.firstChildP;
       eP != NULL;
       eP = indexed ? NULL : eP->next)
  {
    CorNode* idP = corTreeLookup(eP, "id");
    if (idP != NULL && idP->type == CorString && strcmp(idP->value.s, entityId) == 0)
    {
      corDbApplyReportToLive(eP, mergedEntity, reportP);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

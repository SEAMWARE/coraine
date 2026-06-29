//
// FILE            ramdbEntityMerge.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// swRamDB change-set persistence for Merge Entity / Partial Attribute Update.
//
// The NGSI-LD merge itself is done by the broker against the request-arena tree
// returned by db.entityRetrieve; this file applies the resulting change report
// to the LIVE stored entity. Only the attributes the report names are touched —
// a PATCH on one attribute of a 2000-attribute entity does not re-clone the
// whole entity.
//
// The tenant store uses a malloc-backed allocator, so any node grafted into the
// live tree is cloned with the NULL (malloc) allocator; replaced/removed nodes
// are kjFree'd.
//

#include <string.h>                                   // strcmp

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjFree.h"                             // kjFree
#include "kjson/kjBuilder.h"                          // kjChildRemove, kjChildAdd
#include "kjson/kjChildReplace.h"                     // kjChildReplace

#include "swNgsild/LdVocab.h"                         // LD_VOCAB_MODIFIED_AT, LD_VOCAB_SCOPE
#include "swNgsild/ldEntityMerge.h"                   // LdMergeReport

#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, DB_INVALID_GEOMETRY, Tenant
#include "shared/geoMatch.h"                          // geoEntityValidate
#include "currentState/swRamDB/ramdbStore.h"          // ramdbEntities
#include "currentState/swRamDB/ramdbEntityMerge.h"    // Own interface



// -----------------------------------------------------------------------------
//
// replaceOrAdd - graft a malloc-clone of `srcNode` into `live` under `name`,
// replacing (and freeing) any existing same-named child.
//
static void replaceOrAdd(KjNode* live, const char* name, KjNode* srcNode)
{
  if (srcNode == NULL)
    return;

  KjNode* clone = kjClone(NULL, srcNode);  // NULL allocator == malloc == store lifetime
  KjNode* old   = kjLookup(live, name);

  if (old != NULL)
  {
    kjChildReplace(live, old, clone);
    kjFree(old);
  }
  else
    kjChildAdd(live, clone);
}



// -----------------------------------------------------------------------------
//
// ramdbApplyReportToLive - apply a merge report to a live stored entity.
//
// `merged` is the already-merged request-arena tree the report was produced
// against; the new attribute wrappers (and refreshed modifiedAt/type/scope) are
// copied from it into `live`. Shared by the single-entity and batch paths.
//
void ramdbApplyReportToLive(KjNode* live, KjNode* merged, LdMergeReport* reportP)
{
  bool anyChange = false;

  if (reportP != NULL && reportP->changes != NULL)
  {
    for (KjNode* change = reportP->changes->value.firstChildP; change != NULL; change = change->next)
    {
      KjNode* attrNameP = kjLookup(change, "attr");
      KjNode* reasonP   = kjLookup(change, "reason");

      if (attrNameP == NULL || reasonP == NULL || attrNameP->type != KjString || reasonP->type != KjString)
        continue;

      const char* attrName = attrNameP->value.s;
      const char* reason   = reasonP->value.s;

      if (strcmp(reason, "attributeDeleted") == 0)
      {
        KjNode* old = kjLookup(live, attrName);
        if (old != NULL)
        {
          kjChildRemove(live, old);
          kjFree(old);
          anyChange = true;
        }
      }
      else
      {
        replaceOrAdd(live, attrName, kjLookup(merged, attrName));
        anyChange = true;
      }
    }
  }

  if (anyChange)
  {
    replaceOrAdd(live, LD_VOCAB_MODIFIED_AT, kjLookup(merged, LD_VOCAB_MODIFIED_AT));
    replaceOrAdd(live, "type",               kjLookup(merged, "type"));
    replaceOrAdd(live, LD_VOCAB_SCOPE,        kjLookup(merged, LD_VOCAB_SCOPE));
  }
}



// -----------------------------------------------------------------------------
//
// ramdbEntityChangesApply - persist a merged single entity (DB driver entry)
//
int ramdbEntityChangesApply(Tenant* tenantP, const char* entityId,
                            KjNode* mergedEntity, LdMergeReport* reportP)
{
  // Re-validate the GeoProperty values of the COMPLETE merged entity before it
  // touches the store. A PATCH/merge fragment that omits the attribute type is
  // validated as a plain Property (geo check skipped), so a wholesale-replaced
  // GeoProperty value such as {"type":"Polygon"} (no coordinates) would slip
  // through and persist as broken geometry. Same DB_INVALID_GEOMETRY → 400
  // contract as create.
  if (!geoEntityValidate(mergedEntity))
    return DB_INVALID_GEOMETRY;

  KjNode* entities = ramdbEntities(tenantP);

  for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
  {
    KjNode* idP = kjLookup(eP, "id");
    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, entityId) == 0)
    {
      ramdbApplyReportToLive(eP, mergedEntity, reportP);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}

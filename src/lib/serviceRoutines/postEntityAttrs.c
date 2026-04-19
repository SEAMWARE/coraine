//
// FILE            postEntityAttrs.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/entities/{entityId}/attrs — Append Attributes (§ 5.6.3).
//

#include <stddef.h>                                   // NULL
#include <string.h>                                   // strcmp, strlen

#include "swRest/SwRestState.h"                       // swRest
#include "kalloc/kaAlloc.h"                           // kaAlloc
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjObject, kjArray, kjString, kjChildAdd, kjChildRemove
#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/kjClone.h"                            // kjClone

#include "swJsonld/swldCompact.h"                     // swldCompact
#include "swJsonld/swldInit.h"                        // swldCoreContext

#include "swNgsild/swNgsild.h"                        // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldCheckEntity.h"                   // ldCheckEntity
#include "swNgsild/LdOp.h"                            // LdOpAppendAttrs
#include "swNgsild/ldApiEntityToDbModel.h"            // ldApiEntityToDbModel
#include "swNgsild/ldEntityMerge.h"                   // LdMergeReport
#include "swNgsild/LdVocab.h"                         // LD_VOCAB_*
#include "swNgsild/LdSubCache.h"                      // LdSubCache
#include "swNgsild/ldSubscriptionNotify.h"            // LdNotifyEntityUpdate
#include "swNgsild/ldNotifyDefer.h"                   // ldNotifyDefer

#include "db/DbDriver.h"                              // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                                // Tenant

#include "serviceRoutines/postEntityAttrs.h"          // Own interface



// -----------------------------------------------------------------------------
//
// isEntityKeyword - top-level node names that are not attributes
//
static bool isEntityKeyword(const char* name)
{
  if (name == NULL)              return true;
  if (name[0] == '@')            return true;
  if (strcmp(name, "id")   == 0) return true;
  if (strcmp(name, "type") == 0) return true;
  if (strcmp(name, LD_VOCAB_SCOPE) == 0) return true;
  return false;
}



// -----------------------------------------------------------------------------
//
// shortNameOf - compact a (possibly expanded) attribute name for the response
//
static const char* shortNameOf(const char* attrIri)
{
  const char* compact = swldCompact(swldCoreContext(), attrIri);
  return (compact != NULL) ? compact : attrIri;
}



// -----------------------------------------------------------------------------
//
// classifyAndChop - split fragment into "will-apply" vs "notUpdated"
//
// Walks the fragment's top-level attributes. For each attribute instance
// that conflicts with the existing entity at (attrName, dsKey):
//   - noOverwrite=true  → detach the instance from the fragment and record
//                         the attribute name in notUpdatedP (reason
//                         "Attribute already exists").
//   - noOverwrite=false → leave the instance in place (it'll be overwritten
//                         by db.entityAttrsSet).
// Every attribute whose any instance remains in fragment is added to
// updatedP.
//
// Wrappers that end up empty (all instances stripped) are also detached.
// type and scope keywords are passed through — they're not "attributes"
// and don't go into updated[] or notUpdated[].
//
static void classifyAndChop(KjNode* fragment, KjNode* existing, bool noOverwrite,
                            KjNode* updatedP, KjNode* notUpdatedP)
{
  if (fragment == NULL || existing == NULL) return;

  KjNode* fAttrP = fragment->value.firstChildP;
  while (fAttrP != NULL)
  {
    KjNode* nextAttr = fAttrP->next;
    if (isEntityKeyword(fAttrP->name) || fAttrP->type != KjObject)
    {
      fAttrP = nextAttr;
      continue;
    }

    KjNode*     tAttrP      = kjLookup(existing, fAttrP->name);
    const char* shortName   = shortNameOf(fAttrP->name);
    bool        anyConflict = false;
    bool        anyKept     = false;

    if (tAttrP != NULL)
    {
      // Walk instances — detach conflicting ones only when noOverwrite.
      KjNode* fInstP = fAttrP->value.firstChildP;
      while (fInstP != NULL)
      {
        KjNode* nextInst = fInstP->next;
        if (fInstP->type == KjObject)
        {
          KjNode* tInstP = kjLookup(tAttrP, fInstP->name);
          if (tInstP != NULL)
          {
            anyConflict = true;
            if (noOverwrite)
            {
              kjChildRemove(fAttrP, fInstP);
              fInstP = nextInst;
              continue;
            }
          }
        }
        anyKept = true;
        fInstP = nextInst;
      }
    }
    else
    {
      // All instances are new for this attr
      anyKept = true;
    }

    if (noOverwrite && anyConflict)
    {
      // Record as not-updated (even if some instances kept — simplest grain)
      KjNode* entry = kjObject(swRest.kjsonP, NULL);
      kjChildAdd(entry, kjString(swRest.kjsonP, "attributeName", shortName));
      kjChildAdd(entry, kjString(swRest.kjsonP, "reason", "Attribute already exists"));
      kjChildAdd(notUpdatedP, entry);
    }

    if (!anyKept)
    {
      // Fragment's attr now empty — detach the wrapper so the DB op
      // doesn't see an empty set.
      kjChildRemove(fragment, fAttrP);
    }
    else
    {
      kjChildAdd(updatedP, kjString(swRest.kjsonP, NULL, shortName));
    }

    fAttrP = nextAttr;
  }
}



// -----------------------------------------------------------------------------
//
// postEntityAttrs -
//
bool postEntityAttrs(void)
{
  if (swNgsild.contextError)
    return true;

  const char* entityId = swRest.in.wildcard[0];
  KjNode*     fragment = swRest.in.requestTree;

  if (swRest.in.payload != NULL && fragment == NULL)
  {
    ldError(415, LD_ERROR_INVALID_REQUEST, "Unsupported Media Type",
            "supported Content-Types: application/json, application/ld+json");
    return true;
  }

  if (fragment == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "no payload");
    return true;
  }

  if (ldCheckEntity(fragment, LdOpAppendAttrs, NULL, &swRest.kalloc) == false)
    return true;

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  //
  // Retrieve existing entity — needed for 404 + attr/dsKey classification
  // + subscription pre-image payload. The retrieved tree is the live
  // stored tree (ramdb) or an allocated copy (mongoc).
  //
  KjNode* existing = NULL;
  int     rr       = db.entityRetrieve(tenantP, entityId, &existing);

  if (rr == DB_NOT_FOUND || existing == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity '%s' not found", entityId);
    return true;
  }

  if (rr != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "database error retrieving entity '%s'", entityId);
    return true;
  }

  //
  // Fragment → storage form (dataset-keyed wrappers). After this the
  // fragment has the same shape as the existing entity, so we can
  // classify per (attrName, dsKey).
  //
  ldApiEntityToDbModel(fragment, &swRest.kalloc);

  KjNode* updatedP    = kjArray(swRest.kjsonP, "updated");
  KjNode* notUpdatedP = kjArray(swRest.kjsonP, "notUpdated");

  classifyAndChop(fragment, existing, swNgsild.noOverwrite, updatedP, notUpdatedP);

  //
  // Apply the (possibly chopped) fragment. When noOverwrite=true the
  // fragment now contains only attrs/instances that are actually new —
  // the "set or append" semantic degenerates to plain "append" for them.
  // When noOverwrite=false (default), the fragment carries all attrs and
  // entityAttrsSet handles the replace-or-append branching internally.
  //
  if (db.entityAttrsSet == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented",
            "Append Attributes not supported by this DB plugin");
    return true;
  }

  // Scope replace semantics per § 5.6.3.4: overwrite allowed → replace scope;
  // otherwise union. Default (noOverwrite absent) → replace.
  bool overwriteScope = !swNgsild.noOverwrite;

  LdMergeReport report = { NULL };
  int r = db.entityAttrsSet(tenantP, entityId, fragment, overwriteScope,
                             swRest.requestStartTime, &report);

  if (r == DB_NOT_FOUND)
  {
    // Race with concurrent delete — client still gets 404.
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity '%s' not found", entityId);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "database error appending to entity '%s'", entityId);
    return true;
  }

  //
  // Subscription notification — retrieve post-merge state to hand the
  // notifier a complete entity snapshot.
  //
  if (tenantP != NULL && tenantP->subCacheP != NULL)
  {
    KjNode* mergedEntity = NULL;
    db.entityRetrieve(tenantP, entityId, &mergedEntity);
    if (mergedEntity != NULL)
      ldNotifyDefer((LdSubCache*) tenantP->subCacheP, mergedEntity, LdNotifyEntityUpdate, &report);
  }

  //
  // Response decision:
  //   - nothing in notUpdated[] → 204 No Content.
  //   - something in notUpdated[] → 207 Multi-Status + UpdateResult body.
  //
  int notUpdatedCount = 0;
  for (KjNode* p = notUpdatedP->value.firstChildP; p != NULL; p = p->next) notUpdatedCount++;

  if (notUpdatedCount == 0)
  {
    swRest.out.httpStatusCode = 204;
    return true;
  }

  KjNode* respBodyP = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(respBodyP, updatedP);
  kjChildAdd(respBodyP, notUpdatedP);

  swRest.out.responseTree   = respBodyP;
  swRest.out.httpStatusCode = 207;
  return true;
}

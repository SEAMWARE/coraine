//
// FILE            patchSubscription.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                  // NULL
#include <stdlib.h>                                  // free
#include <string.h>                                  // strcmp

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjBuilder.h"                         // kjString, kjChildAdd
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldCheckSubscription.h"            // ldCheckSubscription
#include "swNgsild/ldCheckDateTime.h"                // ldIsoToNanoseconds
#include "swNgsild/LdOp.h"                           // LdOpUpdateSubscription
#include "swNgsild/LdVocab.h"                        // LD_VOCAB_*
#include "swNgsild/LdPernotCache.h"                  // LdPernotCache
#include "swNgsild/ldPernotCache.h"                  // ldPernotCacheItemLookup
#include "swNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem, LdSubSubordinate
#include "swNgsild/ldSubCache.h"                     // ldSubCacheItemRemove, ldSubCacheItemAdd
#include "swNgsild/LdRegCache.h"                     // LdRegCache
#include "swNgsild/ldDistSub.h"                      // ldDistSubReconcile, ldDistSubSubordinatesFragment
#include "swNgsild/ldRegSubMerge.h"                  // ldRegSubMerge
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "swNgsild/SwNgsild.h"                       // ldLocalOnly

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/patchSubscription.h"       // Own interface



//
// distSubPersist - JSON-merge-patch _subordinates onto the sub doc
//
static void distSubPersist(LdSubCacheItem* itemP, void* userData)
{
  if (itemP == NULL || itemP->subId == NULL || db.subscriptionUpdate == NULL)
    return;

  Tenant* tP    = (Tenant*) userData;
  KjNode* fragP = ldDistSubSubordinatesFragment(itemP, swRest.kjsonP);
  if (fragP == NULL)
    return;

  db.subscriptionUpdate(tP, itemP->subId, fragP);
}



// -----------------------------------------------------------------------------
//
// mandatoryAfterMerge - § 5.8.3 / § 5.2.12: the merge must not strip the
// subscription below its mandatory shape. The fragment validator already
// rejects a direct delete of 'notification' or 'notification.endpoint' (a
// urn:ngsi-ld:null there fails the object check), but deleting
// 'notification.endpoint.uri' or deleting the whole entity/attribute selector
// only becomes visible in the MERGED result. A valid Subscription always has a
// 'notification.endpoint.uri' and at least one of 'entities' /
// 'watchedAttributes' — a PATCH must not be able to persist one that lacks them.
//
static bool mandatoryAfterMerge(KjNode* subP)
{
  KjNode* notifP = kjLookup(subP, LD_VOCAB_NOTIFICATION);
  KjNode* epP    = (notifP != NULL) ? kjLookup(notifP, LD_VOCAB_ENDPOINT) : NULL;
  KjNode* uriP   = (epP    != NULL) ? kjLookup(epP, LD_VOCAB_URI)         : NULL;

  if (uriP == NULL || uriP->type != KjString || uriP->value.s[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Subscription",
            "'notification.endpoint.uri' is mandatory and cannot be deleted");
    return false;
  }

  if (kjLookup(subP, LD_VOCAB_ENTITIES) == NULL && kjLookup(subP, LD_VOCAB_WATCHED_ATTRS) == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Subscription",
            "Subscription must have 'entities' or 'watchedAttributes'");
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// patchSubscription -
//
bool patchSubscription(void)
{
  const char* subId    = swRest.in.wildcard[0];
  KjNode*     fragment = swRest.in.requestTree;
  //
  // PATCH body needs at least one updatable field. § 5.2.12: id and type
  // are read-only; ldParseHook tolerates a `type:"Subscription"` echo for
  // ETSI 029-style PATCHes that carry it for completeness, but a PATCH
  // body whose ONLY contents are id/type is the ETSI 028_05/06 "tried to
  // modify the read-only field" case — surface a 400 with the precise
  // ProblemDetails the test expects.
  //
  if (fragment != NULL && fragment->type == KjObject)
  {
    bool hasUpdatable = false;
    bool sawType      = false;
    bool sawId        = false;
    for (KjNode* c = fragment->value.firstChildP; c != NULL; c = c->next)
    {
      if (c->name == NULL)                 continue;
      if (strcmp(c->name, "type") == 0)  { sawType = true; continue; }
      if (strcmp(c->name, "id")   == 0)  { sawId   = true; continue; }
      hasUpdatable = true;
    }
    //
    // 'id' is immutable: present in a PATCH at all — even alongside updatable
    // fields, even as a urn:ngsi-ld:null delete — is a 400, not a silently
    // ignored member. (Don't early-break the scan above, or an updatable field
    // following 'id' would mask it.)
    //
    if (sawId)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Read-Only Field", "'id' cannot be modified");
      return true;
    }
    if (!hasUpdatable)
    {
      if (sawType)
        ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Read-Only Field",
                "'type' cannot be modified");
      else
        ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Subscription",
                "PATCH body must contain at least one updatable field");
      return true;
    }
  }

  //
  // Validate the subscription fragment
  //
  if (ldCheckSubscription(fragment, LdOpUpdateSubscription, &swRest.kalloc) == false)
    return true;

  //
  // Per spec 5.8.2.4: expiresAt in the past is an error
  //
  KjNode* expiresAtP = kjLookup(fragment, LD_VOCAB_EXPIRES_AT);
  if (expiresAtP != NULL && expiresAtP->type == KjString)
  {
    uint64_t expiresNs = ldIsoToNanoseconds(expiresAtP->value.s);
    if (expiresNs > 0 && expiresNs < swRest.requestStartTime)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Subscription",
              "'expiresAt' must be a DateTime in the future");
      return true;
    }
  }

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  //
  // Block patching of CSR-subs via this endpoint.
  //
  if (tenantP->regSubCacheP != NULL
      && ldSubCacheItemLookup((LdSubCache*) tenantP->regSubCacheP, subId) != NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "subscription '%s' not found", subId);
    return true;
  }

  //
  // Can't switch between periodic (timeInterval) and normal (watchedAttributes)
  //
  KjNode* tiInFragment = kjLookup(fragment, "timeInterval");
  KjNode* waInFragment = kjLookup(fragment, LD_VOCAB_WATCHED_ATTRS);
  KjNode* thInFragment = kjLookup(fragment, LD_VOCAB_THROTTLING);

  bool existingIsPernot = (tenantP->pernotCacheP != NULL &&
                           ldPernotCacheItemLookup((LdPernotCache*) tenantP->pernotCacheP, subId) != NULL);

  if (tiInFragment != NULL && !existingIsPernot)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Subscription",
            "cannot add 'timeInterval' to a non-periodic subscription");
    return true;
  }
  if (existingIsPernot && (waInFragment != NULL || thInFragment != NULL))
  {
    const char* field = (waInFragment != NULL) ? "watchedAttributes" : "throttling";
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Subscription",
            "cannot add '%s' to a periodic (timeInterval) subscription", field);
    return true;
  }

  //
  // Update Subscription (§ 5.8.3) — the broker owns the merge. Load the current
  // subscription, apply the fragment here (JSON Merge Patch; urn:ngsi-ld:null
  // members, already resolved to KjNull by the validator, delete their target),
  // and hand the DB plugin a complete document to store. Keeping the NGSI-LD
  // merge/delete semantics in the broker means every DB plugin is a dumb store.
  //
  // The whole retrieve → merge → store → cache-refresh runs under the sub-cache
  // write lock so concurrent sub writers serialize (the read-modify-write is not
  // atomic at the DB on its own). The reg-touching reconcile runs AFTER we drop
  // the lock (lock order reg-before-sub), with newItemP pinned so a concurrent
  // sub DELETE can't free it during the reconcile's remote calls.
  //
  if (db.subscriptionReplace == NULL || db.subscriptionRetrieve == NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented", "subscription CRUD not supported by this DB plugin");
    return true;
  }

  // The subordinate-sub mapping (§ 5.8.1.4) lives only on the cache item; it
  // would be lost on remove. Detach + reattach across the rebuild.
  LdSubSubordinate* savedSubordinateP     = NULL;
  int               savedSubordinateRunNo = 0;
  LdSubCache*       subCacheP = (LdSubCache*) tenantP->subCacheP;
  LdSubCacheItem*   newItemP  = NULL;

  ldSubCacheWrLock(subCacheP);

  KjNode* mergedSubP = NULL;
  int     rr         = db.subscriptionRetrieve(tenantP, subId, &mergedSubP);

  if (rr == DB_NOT_FOUND || mergedSubP == NULL)
  {
    ldSubCacheUnlock(subCacheP);
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "subscription '%s' not found", subId);
    return true;
  }

  if (rr != DB_OK)
  {
    ldSubCacheUnlock(subCacheP);
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error retrieving subscription '%s'", subId);
    return true;
  }

  ldRegSubMerge(mergedSubP, fragment, swRest.kjsonP);

  //
  // § 5.8.3 — re-validate the mandatory shape of the merged result before
  // persisting: a PATCH that deletes endpoint.uri or the whole entity/attribute
  // selector would otherwise store an unusable subscription (the fragment
  // validator can't see those holes — they only exist post-merge).
  //
  if (mandatoryAfterMerge(mergedSubP) == false)
  {
    ldSubCacheUnlock(subCacheP);
    return true;  // mandatoryAfterMerge already raised the 400
  }

  //
  // Recompute status (§ 5.8.2.4) from the merged isActive + expiresAt, into the
  // merged document, so the single store below persists it (no extra $set).
  //
  {
    KjNode* isActiveP = kjLookup(mergedSubP, LD_VOCAB_IS_ACTIVE);
    KjNode* expiresP  = kjLookup(mergedSubP, LD_VOCAB_EXPIRES_AT);
    KjNode* statusP   = kjLookup(mergedSubP, LD_VOCAB_STATUS);
    bool    isActive  = (isActiveP == NULL || isActiveP->type != KjBoolean || isActiveP->value.b == true);
    bool    isExpired = false;

    if (expiresP != NULL && expiresP->type == KjString)
    {
      uint64_t expiresNs = ldIsoToNanoseconds(expiresP->value.s);
      if (expiresNs > 0 && expiresNs < swRest.requestStartTime)
        isExpired = true;
    }

    const char* newStatus = isExpired ? "expired" : (isActive ? "active" : "paused");

    if (statusP != NULL && statusP->type == KjString)
      statusP->value.s = (char*) newStatus;
    else
      kjChildAdd(mergedSubP, kjString(swRest.kjsonP, LD_VOCAB_STATUS, newStatus));
  }

  int r = db.subscriptionReplace(tenantP, subId, mergedSubP);

  if (r != DB_OK)
  {
    ldSubCacheUnlock(subCacheP);
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error updating subscription '%s'", subId);
    return true;
  }

  // Refresh the cache from the merged document we just stored — no second retrieve.
  if (subCacheP != NULL)
  {
    LdSubCacheItem* oldItemP = ldSubCacheItemLookup(subCacheP, subId);
    if (oldItemP != NULL)
    {
      savedSubordinateP     = oldItemP->subordinateP;
      savedSubordinateRunNo = oldItemP->subordinateRunNo;
      oldItemP->subordinateP = NULL;   // detach so cache-free won't release
    }

    ldSubCacheItemRemove(subCacheP, subId);

    newItemP = ldSubCacheItemAdd(subCacheP, mergedSubP, NULL);

    // Reattach the subordinate mapping that survived the cache rebuild.
    if (newItemP != NULL && savedSubordinateP != NULL)
    {
      // ldSubCacheItemAdd may have parsed a subordinate list from the persisted
      // mergedSubP; the live (saved) mapping is authoritative, so free the
      // freshly-parsed one before overwriting — else it is orphaned (leak).
      ldSubCacheSubordinatesFree(newItemP->subordinateP);
      newItemP->subordinateP     = savedSubordinateP;
      newItemP->subordinateRunNo = savedSubordinateRunNo;
      savedSubordinateP          = NULL;   // ownership transferred
    }

    if (newItemP != NULL)
      ldSubCacheItemPin(newItemP);
  }
  ldSubCacheUnlock(subCacheP);

  // § 5.8.1.4 — reconcile the subordinate set against the patched entity filter
  // (PATCH still-matching derivatives, DELETE no-longer-overlapping ones, fan
  // out new matches). Touches the reg cache + remote I/O — lock-free, pinned.
  // Best-effort — remote failures don't roll back local state.
  if (newItemP != NULL && tenantP->regCacheP != NULL && !ldLocalOnly)
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);
    ldDistSubReconcile(newItemP, fragment, (LdRegCache*) tenantP->regCacheP, ownAlias,
                       distSubPersist, tenantP);
  }
  if (newItemP != NULL)
    ldSubCacheItemUnpin(newItemP);

  // If the cache rebuild bailed before re-adding (DB retrieve failure),
  // the saved subordinate list still needs freeing — its ownership was
  // detached from the now-removed cache item.
  if (savedSubordinateP != NULL)
  {
    LdSubSubordinate* sub = savedSubordinateP;
    while (sub != NULL)
    {
      LdSubSubordinate* next = sub->next;
      free(sub->remoteSubId);
      free(sub->regId);
      free(sub);
      sub = next;
    }
  }

  swRest.out.httpStatusCode = 204;
  return true;
}

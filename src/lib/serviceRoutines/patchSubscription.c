//
// FILE            patchSubscription.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stddef.h>                                  // NULL
#include <stdlib.h>                                  // free
#include <string.h>                                  // strcmp

#include "corRest/CorRestState.h"                      // corRest
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjBuilder.h"                         // kjString, kjChildAdd
#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/ldCheckSubscription.h"            // ldCheckSubscription
#include "corNgsild/ldCheckDateTime.h"                // ldIsoToNanoseconds
#include "corNgsild/LdOp.h"                           // LdOpUpdateSubscription
#include "corNgsild/LdVocab.h"                        // LD_VOCAB_*
#include "corNgsild/LdPernotCache.h"                  // LdPernotCache
#include "corNgsild/ldPernotCache.h"                  // ldPernotCacheItemLookup
#include "corNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem, LdSubSubordinate
#include "corNgsild/ldSubCache.h"                     // ldSubCacheItemRemove, ldSubCacheItemAdd
#include "corNgsild/ldSysTimestamp.h"                 // ldSysTimestampModify
#include "corNgsild/LdRegCache.h"                     // LdRegCache
#include "corNgsild/ldDistSub.h"                      // ldDistSubReconcile, ldDistSubSubordinatesFragment
#include "corNgsild/ldRegSubMerge.h"                  // ldRegSubMerge
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "corNgsild/CorNgsild.h"                       // ldDistributed

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
  KjNode* fragP = ldDistSubSubordinatesFragment(itemP, corRest.kjsonP);
  if (fragP == NULL)
    return;

  db.subscriptionUpdate(tP, itemP->subId, fragP);
}



// -----------------------------------------------------------------------------
//
// patchSubscription -
//
bool patchSubscription(void)
{
  const char* subId    = corRest.in.wildcard[0];
  KjNode*     fragment = corRest.in.requestTree;
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
  // Fragment validation only (well-formedness). The format value isn't parsed
  // here — it's validated and captured from the merged result below, so the
  // string is matched once. NULL: nothing to capture from the fragment.
  if (ldCheckSubscription(fragment, LdOpUpdateSubscription, /*merged*/false, NULL, &corRest.kalloc) == false)
    return true;

  //
  // Per spec 5.8.2.4: expiresAt in the past is an error
  //
  KjNode* expiresAtP = kjLookup(fragment, LD_VOCAB_EXPIRES_AT);
  if (expiresAtP != NULL && expiresAtP->type == KjString)
  {
    uint64_t expiresNs = ldIsoToNanoseconds(expiresAtP->value.s);
    if (expiresNs > 0 && expiresNs < corRest.requestStartTime)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Subscription",
              "'expiresAt' must be a DateTime in the future");
      return true;
    }
  }

  Tenant* tenantP = (Tenant*) corNgsild.tenantP;

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

  ldRegSubMerge(mergedSubP, fragment, corRest.kjsonP);

  //
  // § 5.8.3 — re-validate the COMPLETE merged result before persisting. The
  // fragment validator only sees the members the PATCH carried; invariants that
  // span the whole document (a deleted endpoint.uri, a deleted entity/attribute
  // selector, a deleted notification) only become visible post-merge. Running
  // the Create-grade validator in 'merged' mode enforces the full mandatory /
  // consistency shape while tolerating the server-owned fields (status, …) the
  // stored document legitimately carries.
  //
  LdFormat notifFormat = LdFormatNone;
  if (ldCheckSubscription(mergedSubP, LdOpCreateSubscription, /*merged*/true, &notifFormat, &corRest.kalloc) == false)
  {
    ldSubCacheUnlock(subCacheP);
    return true;  // ldCheckSubscription already raised the 400
  }

  //
  // A periodic subscription must keep its 'timeInterval'. The Create-grade
  // validator doesn't require one (a normal subscription has none), so a PATCH
  // that deletes timeInterval from a periodic sub would otherwise slip through
  // and leave it neither periodic nor watch-driven — guard it explicitly.
  //
  if (existingIsPernot && kjLookup(mergedSubP, "timeInterval") == NULL)
  {
    ldSubCacheUnlock(subCacheP);
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Subscription",
            "'timeInterval' cannot be deleted from a periodic subscription");
    return true;
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
      if (expiresNs > 0 && expiresNs < corRest.requestStartTime)
        isExpired = true;
    }

    const char* newStatus = isExpired ? "expired" : (isActive ? "active" : "paused");

    if (statusP != NULL && statusP->type == KjString)
      statusP->value.s = (char*) newStatus;
    else
      kjChildAdd(mergedSubP, kjString(corRest.kjsonP, LD_VOCAB_STATUS, newStatus));
  }

  // § 6.4.5 — bump modifiedAt to now; createdAt (from the retrieved tree) stays
  ldSysTimestampModify(mergedSubP);

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

    newItemP = ldSubCacheItemAdd(subCacheP, mergedSubP, NULL, notifFormat);

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
  if (newItemP != NULL && tenantP->regCacheP != NULL && ldDistributed)
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);
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

  corRest.out.httpStatusCode = 204;
  return true;
}

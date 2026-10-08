//
// FILE            patchCsourceSubscription.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// PATCH /ngsi-ld/v1/csourceSubscriptions/{id}  (NGSI-LD § 5.11.3)
//
// JSON Merge Patch semantics applied to the cached subscription tree.
// Cache-only in v1 (no DB persistence). After the patch is applied the
// cache item is removed and re-added so the parsed shortcuts (q,
// entitySelectors, geoRel, …) are rebuilt.
//
// Per § 5.11.3.4, a notification with all currently matching CSRs
// must be sent after the patch — triggered by the CSR-sub
// matcher/notifier (hooked in follow-up commit).
//
#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp

#include "corRest/CorRestState.h"                      // corRest
#include "corTree/CorNode.h"                         // CorNode, CorNull
#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corTree/corTreeBuilder.h"                  // corTreeChildAdd, corTreeChildRemove, corTreeString
#include "corTree/corTreeFree.h"                     // corTreeFree
#include "corTree/corTreeClone.h"                    // corTreeClone

#include "corNgsild/corNgsild.h"                       // ldError, corNgsild
#include "corNgsild/ldCheckSubscription.h"            // ldCheckSubscription
#include "corNgsild/ldCheckDateTime.h"                // ldIsoToNanoseconds
#include "corNgsild/LdOp.h"                           // LdOpUpdateCsourceSubscription
#include "corNgsild/LdVocab.h"                        // LD_VOCAB_*
#include "corNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem
#include "corNgsild/ldSubCache.h"                     // ldSubCacheItemLookup, ldSubCacheItemRemove, ldSubCacheItemAdd
#include "corNgsild/ldSysTimestamp.h"                 // ldSysTimestampModify
#include "corNgsild/LdRegCache.h"                     // LdRegCache
#include "corNgsild/ldCsrSubNotify.h"                 // ldCsrSubInitialNotify
#include "corNgsild/ldRegSubMerge.h"                  // ldRegSubMerge

#include "db/DbDriver.h"                             // db, DB_OK
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/patchCsourceSubscription.h"  // Own interface



bool patchCsourceSubscription(void)
{
  const char* subId    = corRest.in.wildcard[0];
  CorNode*    fragment = corRest.in.requestTree;

  // The fragment first; the complete result once applied (below). The format is re-derived from the
  // tree at cache time (LdFormatUnset below).
  if (ldCheckSubscription(fragment, LdOpUpdateCsourceSubscription, /*merged*/false, NULL, &corRest.kalloc) == false)
    return true;

  // expiresAt-past check
  CorNode* expiresAtP = corTreeLookup(fragment, LD_VOCAB_EXPIRES_AT);
  if (expiresAtP != NULL && expiresAtP->type == CorString)
  {
    uint64_t expiresNs = ldIsoToNanoseconds(expiresAtP->value.s);
    if (expiresNs > 0 && expiresNs < corRest.requestStartTime)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Subscription",
              "'expiresAt' must be a DateTime in the future");
      return true;
    }
  }

  // timeInterval not supported for CSR subs (defer to follow-up)
  if (corTreeLookup(fragment, "timeInterval") != NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
            "periodic CSR subscriptions ('timeInterval') are not supported");
    return true;
  }

  Tenant*     tenantP = (Tenant*) corNgsild.tenantP;
  LdSubCache* cacheP  = (LdSubCache*) tenantP->regSubCacheP;

  // The whole Lookup → update → Remove + Add is one read-modify-write on the CSR-sub cache, so hold
  // the wrlock across all of it. The
  // reg-touching notify runs AFTER we pin newItemP and drop the lock
  // (lock order reg-before-sub).
  ldSubCacheWrLock(cacheP);

  LdSubCacheItem* itemP = (cacheP != NULL) ? ldSubCacheItemLookup(cacheP, subId) : NULL;

  if (itemP == NULL || itemP->subTree == NULL)
  {
    ldSubCacheUnlock(cacheP);
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "CSR subscription '%s' not found", subId);
    return true;
  }

  //
  // The partial update (TS 104-175 § 12.4.3.4 -> § 8.4.2): each first-level member of the fragment
  // replaces, adds or deletes - on a copy, so the cached subscription is untouched until the complete
  // result is known valid. A fragment's "notification" is the whole new notification: without an
  // endpoint it is a 400, not a subscription stored without one.
  //
  CorNode* subTree = corTreeClone(corRest.kallocP, itemP->subTree);

  ldRegSubMerge(subTree, fragment, corRest.kallocP);

  if (ldCheckSubscription(subTree, LdOpCreateCsourceSubscription, /*merged*/true, NULL, &corRest.kalloc) == false)
  {
    ldSubCacheUnlock(cacheP);
    return true;  // ldCheckSubscription already raised the 400
  }

  //
  // Recompute status from isActive + expiresAt (spec § 5.8.2.4 analogue)
  //
  {
    CorNode* isActiveP = corTreeLookup(subTree, LD_VOCAB_IS_ACTIVE);
    CorNode* expiresP = corTreeLookup(subTree, LD_VOCAB_EXPIRES_AT);
    CorNode* statusP  = corTreeLookup(subTree, LD_VOCAB_STATUS);
    bool    isActive  = (isActiveP == NULL || isActiveP->type != CorBoolean || isActiveP->value.b == true);
    bool    isExpired = false;

    if (expiresP != NULL && expiresP->type == CorString)
    {
      uint64_t expiresNs = ldIsoToNanoseconds(expiresP->value.s);
      if (expiresNs > 0 && expiresNs < corRest.requestStartTime)
        isExpired = true;
    }

    const char* newStatus = isExpired ? "expired" : (isActive ? "active" : "paused");

    if (statusP != NULL && statusP->type == CorString)
      statusP->value.s = (char*) newStatus;
    else
      corTreeChildAdd(subTree, corTreeString(corRest.kallocP, LD_VOCAB_STATUS, newStatus));
  }

  // § 6.4.5 — bump modifiedAt to now (in-place on the existing integer node;
  // createdAt, stamped at create time, is left untouched)
  ldSysTimestampModify(subTree);

  //
  // Persist the merged tree. The DB fragment semantics in our plugins
  // do merge-patch anyway, but since we've already merged in-memory
  // we just push the full updated tree via the same entry point.
  //
  if (db.subscriptionUpdate != NULL)
    db.subscriptionUpdate(tenantP, subId, subTree);

  //
  // Re-add cache item so parsed shortcuts are rebuilt.
  // Preserve the subTree reference by cloning once more via the cache's own path.
  //
  CorNode* newTree = corTreeClone(NULL, subTree);
  ldSubCacheItemRemove(cacheP, subId);
  LdSubCacheItem* newItemP = ldSubCacheItemAdd(cacheP, newTree, NULL, LdFormatUnset);
  corTreeFree(newTree); // ItemAdd deep-clones its input; the intermediate is ours to free

  // Pin the rebuilt item and drop the wrlock before the reg-touching notify.
  if (newItemP != NULL)
    ldSubCacheItemPin(newItemP);
  ldSubCacheUnlock(cacheP);

  //
  // § 5.11.3.4 — "send a notification with all currently matching
  // Context Source Registrations" after the PATCH. Reuses the
  // initial-on-subscribe code path: one CsourceNotification with all
  // matches under the new filter, triggerReason="newlyMatching".
  //
  if (newItemP != NULL && tenantP->regCacheP != NULL)
    ldCsrSubInitialNotify((LdRegCache*) tenantP->regCacheP, newItemP);
  if (newItemP != NULL)
    ldSubCacheItemUnpin(newItemP);

  corRest.out.httpStatusCode = 204;
  return true;
}

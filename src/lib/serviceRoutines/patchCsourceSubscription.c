//
// FILE            patchCsourceSubscription.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
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

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/KjNode.h"                            // KjNode, KjNull
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjBuilder.h"                         // kjChildAdd, kjChildRemove, kjString
#include "kjson/kjClone.h"                           // kjClone

#include "swNgsild/swNgsild.h"                       // ldError, swNgsild
#include "swNgsild/ldCheckSubscription.h"            // ldCheckSubscription
#include "swNgsild/ldCheckDateTime.h"                // ldIsoToNanoseconds
#include "swNgsild/LdOp.h"                           // LdOpUpdateCsourceSubscription
#include "swNgsild/LdVocab.h"                        // LD_VOCAB_*
#include "swNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem
#include "swNgsild/ldSubCache.h"                     // ldSubCacheItemLookup, ldSubCacheItemRemove, ldSubCacheItemAdd
#include "swNgsild/LdRegCache.h"                     // LdRegCache
#include "swNgsild/ldCsrSubNotify.h"                 // ldCsrSubInitialNotify

#include "db/DbDriver.h"                             // db, DB_OK
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/patchCsourceSubscription.h"  // Own interface



bool patchCsourceSubscription(void)
{
  const char* subId    = swRest.in.wildcard[0];
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

  if (ldCheckSubscription(fragment, LdOpUpdateCsourceSubscription, &swRest.kalloc) == false)
    return true;

  // expiresAt-past check
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

  // timeInterval not supported for CSR subs (defer to follow-up)
  if (kjLookup(fragment, "timeInterval") != NULL)
  {
    ldError(501, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
            "periodic CSR subscriptions ('timeInterval') are not supported");
    return true;
  }

  Tenant*     tenantP = (Tenant*) swNgsild.tenantP;
  LdSubCache* cacheP  = (LdSubCache*) tenantP->regSubCacheP;

  LdSubCacheItem* itemP = (cacheP != NULL) ? ldSubCacheItemLookup(cacheP, subId) : NULL;

  if (itemP == NULL || itemP->subTree == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "CSR subscription '%s' not found", subId);
    return true;
  }

  //
  // Apply JSON Merge Patch to itemP->subTree.
  // Fragment field == null → remove; else replace/add (clone to detach).
  //
  KjNode* subTree = itemP->subTree;
  KjNode* next;

  for (KjNode* fieldP = fragment->value.firstChildP; fieldP != NULL; fieldP = next)
  {
    next = fieldP->next;

    KjNode* existingP = kjLookup(subTree, fieldP->name);

    if (fieldP->type == KjNull)
    {
      if (existingP != NULL)
        kjChildRemove(subTree, existingP);
    }
    else
    {
      if (existingP != NULL)
        kjChildRemove(subTree, existingP);

      KjNode* cloneP = kjClone(NULL, fieldP);
      kjChildAdd(subTree, cloneP);
    }
  }

  //
  // Recompute status from isActive + expiresAt (spec § 5.8.2.4 analogue)
  //
  {
    KjNode* isActiveP = kjLookup(subTree, LD_VOCAB_IS_ACTIVE);
    KjNode* expiresP  = kjLookup(subTree, LD_VOCAB_EXPIRES_AT);
    KjNode* statusP   = kjLookup(subTree, LD_VOCAB_STATUS);
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
      kjChildAdd(subTree, kjString(NULL, LD_VOCAB_STATUS, newStatus));
  }

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
  KjNode* newTree = kjClone(NULL, subTree);
  ldSubCacheItemRemove(cacheP, subId);
  LdSubCacheItem* newItemP = ldSubCacheItemAdd(cacheP, newTree, NULL);

  //
  // § 5.11.3.4 — "send a notification with all currently matching
  // Context Source Registrations" after the PATCH. Reuses the
  // initial-on-subscribe code path: one CsourceNotification with all
  // matches under the new filter, triggerReason="newlyMatching".
  //
  if (newItemP != NULL && tenantP->regCacheP != NULL)
    ldCsrSubInitialNotify((LdRegCache*) tenantP->regCacheP, newItemP);

  swRest.out.httpStatusCode = 204;
  return true;
}

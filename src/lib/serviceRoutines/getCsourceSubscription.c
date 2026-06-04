//
// FILE            getCsourceSubscription.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/csourceSubscriptions/{id}  (NGSI-LD § 5.11.4)
//
#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjString, kjChildAdd, kjChildRemove
#include "kjson/kjClone.h"                           // kjClone
#include "kjson/kjLookup.h"                          // kjLookup

#include "swNgsild/swNgsild.h"                       // ldError, ldContextResolve, swNgsild
#include "swNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem
#include "swNgsild/ldSubCache.h"                     // ldSubCacheItemLookup
#include "swNgsild/ldSubscriptionCompactQ.h"         // ldSubscriptionCompactQ
#include "swNgsild/LdSubStatus.h"                    // ldSubStatusToString
#include "swNgsild/ldSubscriptionCounters.h"         // ldSubscriptionCountersInject
#include "swNgsild/LdVocab.h"                         // LD_VOCAB_STATUS, LD_VOCAB_EXPIRES_AT
#include "swNgsild/ldCheckDateTime.h"                 // ldIsoToNanoseconds

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getCsourceSubscription.h"  // Own interface



bool getCsourceSubscription(void)
{
  const char* subId = swRest.in.wildcard[0];

  Tenant*     tenantP = (Tenant*) swNgsild.tenantP;
  LdSubCache* cacheP  = (LdSubCache*) tenantP->regSubCacheP;

  LdSubCacheItem* itemP = (cacheP != NULL) ? ldSubCacheItemLookup(cacheP, subId) : NULL;

  if (itemP == NULL || itemP->subTree == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "CSR subscription '%s' not found", subId);
    return true;
  }

  ldContextResolve();

  KjNode* subP = kjClone(swRest.kjsonP, itemP->subTree);
  ldSubscriptionCompactQ(subP, itemP->qExpr, swNgsild.contextP, &swRest.kalloc);
  ldSubscriptionCountersInject(subP, itemP);

  // Hide the internal marker
  KjNode* kindP = kjLookup(subP, "_subKind");
  if (kindP != NULL)
    kjChildRemove(subP, kindP);

  // § 5.2 Subscription table: `notificationTrigger` is "not applicable
  // and shall be ignored" for CSR-sub. The lib's ldCheckSubscription
  // strips it on create/update, so it's never stored — no work to do
  // here. The entity-sub GET in getSubscription.c still default-emits.

  // Strip the broker-internal `_jcResolved`.
  KjNode* jcP = kjLookup(subP, "_jcResolved");
  if (jcP != NULL)
    kjChildRemove(subP, jcP);

  //
  // § 12.4.7: a failed notification delivery sets the LIVE status to
  // "failed" (cleared again by the next success). The stored doc still
  // says "active" — the cache item is authoritative.
  //
  {
    char*   liveStatus = (char*) ldSubStatusToString(itemP->status);
    KjNode* statusP    = kjLookup(subP, LD_VOCAB_STATUS);
    if (statusP == NULL)
      statusP = kjLookup(subP, "status");
    if (statusP != NULL && statusP->type == KjString)
      statusP->value.s = liveStatus;
    else if (statusP == NULL)
      kjChildAdd(subP, kjString(swRest.kjsonP, "status", liveStatus));
  }

  //
  // § 5.2.12: `status` is read-only and computed. It was stored as
  // "active" / "paused" at create time and never updated. Override
  // it here when `expiresAt` is in the past so retrieve reflects
  // the current lifecycle state.
  //
  KjNode* expiresAtP = kjLookup(subP, LD_VOCAB_EXPIRES_AT);
  if (expiresAtP != NULL && expiresAtP->type == KjString)
  {
    uint64_t expiresNs = ldIsoToNanoseconds(expiresAtP->value.s);
    if (expiresNs > 0 && expiresNs < swRest.requestStartTime)
    {
      KjNode* statusP = kjLookup(subP, LD_VOCAB_STATUS);
      if (statusP != NULL && statusP->type == KjString)
        statusP->value.s = "expired";
    }
  }

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = subP;
  return true;
}

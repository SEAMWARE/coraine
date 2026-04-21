//
// FILE            patchSubscription.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                  // NULL
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
#include "swNgsild/LdSubCache.h"                     // LdSubCache
#include "swNgsild/ldSubCache.h"                     // ldSubCacheItemRemove, ldSubCacheItemAdd

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/patchSubscription.h"       // Own interface



// -----------------------------------------------------------------------------
//
// patchSubscription -
//
bool patchSubscription(void)
{
  //
  // @context error detected in parseHook
  //
  if (swNgsild.contextError)
    return true;

  const char* subId    = swRest.in.wildcard[0];
  KjNode*     fragment = swRest.in.requestTree;

  //
  // Unsupported Content-Type
  //
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
  if (tenantP != NULL && tenantP->regSubCacheP != NULL
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
  // Update subscription in database (JSON Merge Patch)
  //
  if (db.subscriptionUpdate == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented", "subscription CRUD not supported by this DB plugin");
    return true;
  }

  int r = db.subscriptionUpdate((Tenant*) swNgsild.tenantP, subId, fragment);

  if (r == DB_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "subscription '%s' not found", subId);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error updating subscription '%s'", subId);
    return true;
  }

  //
  // Update subscription cache: remove old entry, re-add from DB
  // (re-add retrieves the merged subscription and re-parses all fields)
  //
  if (tenantP->subCacheP != NULL)
  {
    ldSubCacheItemRemove((LdSubCache*) tenantP->subCacheP, subId);

    KjNode* updatedSubP = NULL;
    if (db.subscriptionRetrieve(tenantP, subId, &updatedSubP) == DB_OK && updatedSubP != NULL)
    {
      //
      // Recompute status from isActive + expiresAt (per spec 5.8.2.4)
      //
      KjNode* isActiveP  = kjLookup(updatedSubP, LD_VOCAB_IS_ACTIVE);
      KjNode* expiresP   = kjLookup(updatedSubP, LD_VOCAB_EXPIRES_AT);
      KjNode* statusP    = kjLookup(updatedSubP, LD_VOCAB_STATUS);
      bool    isActive   = (isActiveP == NULL || isActiveP->type != KjBoolean || isActiveP->value.b == true);
      bool    isExpired  = false;

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
        kjChildAdd(updatedSubP, kjString(NULL, LD_VOCAB_STATUS, newStatus));

      // Persist the updated status
      KjNode* statusFragment = kjObject(NULL, NULL);
      kjChildAdd(statusFragment, kjString(NULL, LD_VOCAB_STATUS, newStatus));
      db.subscriptionUpdate(tenantP, subId, statusFragment);

      ldSubCacheItemAdd((LdSubCache*) tenantP->subCacheP, updatedSubP, NULL);
    }
  }

  swRest.out.httpStatusCode = 204;
  return true;
}

//
// FILE            postSubscriptions.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <string.h>                                  // strlen, strcpy, strcat
#include <stdio.h>                                   // snprintf
#include <time.h>                                    // time

#include "swRest/SwRestState.h"                      // swRest
#include "swRest/swRestOutHeader.h"                  // swRestOutHeaderAdd
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjClone.h"                           // kjClone
#include "kjson/kjBuilder.h"                         // kjString, kjChildAdd
#include "kjson/KjNode.h"                            // KjNode, KjString
#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "swJsonld/swldInit.h"                       // swldCoreContext
#include "swJsonld/SwldContext.h"                    // SwldContext
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldCheckSubscription.h"            // ldCheckSubscription
#include "swNgsild/LdOp.h"                           // LdOpCreateSubscription
#include "swNgsild/LdVocab.h"                        // LD_VOCAB_IS_ACTIVE, LD_VOCAB_STATUS
#include "swNgsild/LdSubCache.h"                     // LdSubCache
#include "swNgsild/ldSubCache.h"                     // ldSubCacheItemAdd
#include "swNgsild/ldPernotCache.h"                  // ldPernotCacheItemAdd
#include "swNgsild/ldQParse.h"                       // ldQParse
#include "swNgsild/ldQRender.h"                      // ldQRender
#include "swNgsild/LdRegCache.h"                     // LdRegCache
#include "swNgsild/ldDistSub.h"                      // ldDistSubFanout
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant

#include "db/DbDriver.h"                             // db, DB_OK, DB_ALREADY_EXISTS
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postSubscriptions.h"       // Own interface



// -----------------------------------------------------------------------------
//
// distSubPersist - persist subordinate mapping after a fanout mutation
//
// LdDistSubPersistFunc callback invoked from ldDistSub.c whenever an
// itemP->subordinateP list changes. JSON-merge-patch onto the sub doc
// so the mapping survives a broker restart.
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
// subIdGenerate - generate a subscription id if none provided
//
static char* subIdGenerate(KAlloc* allocP)
{
  static int counter = 0;
  char*      buf     = kaAlloc(allocP, 128);

  snprintf(buf, 128, "urn:ngsi-ld:Subscription:%lx:%04x", (long) time(NULL), ++counter & 0xFFFF);

  return buf;
}



// -----------------------------------------------------------------------------
//
// postSubscriptions -
//
bool postSubscriptions(void)
{
  KjNode* subP = swRest.in.requestTree;

  //
  // Must have a JSON payload
  //
  //
  // Validate the subscription
  //
  if (ldCheckSubscription(subP, LdOpCreateSubscription, &swRest.kalloc) == false)
    return true;

  //
  // Extract or generate subscription id
  //
  KjNode* idP = kjLookup(subP, "id");

  if (idP == NULL)
  {
    char* generatedId = subIdGenerate(&swRest.kalloc);

    idP = kjString(NULL, "id", generatedId);
    kjChildAdd(subP, idP);
  }
  else if (idP->type != KjString)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "subscription 'id' must be a string");
    return true;
  }

  //
  // Reject id collision with an existing CSR-subscription — the mongo
  // collection is shared across /subscriptions and /csourceSubscriptions.
  //
  {
    Tenant* _t = (Tenant*) swNgsild.tenantP;
    if (_t != NULL && _t->regSubCacheP != NULL
        && ldSubCacheItemLookup((LdSubCache*) _t->regSubCacheP, idP->value.s) != NULL)
    {
      ldError(409, LD_ERROR_ALREADY_EXISTS, "Already Exists",
              "subscription '%s' already exists", idP->value.s);
      return true;
    }
  }

  //
  // Expand q-filter attribute names using the request's @context.
  // The q string is opaque to JSON-LD expansion, so we parse it (which expands
  // attr names via swNgsild.contextP), then render back to a string with the
  // expanded IRIs and replace the value in the subscription tree.
  //
  //
  // Expand q-filter and store expanded version + pre-parsed tree for the cache.
  // Parse once with the cache's allocator so the tree persists across requests.
  //
  LdQNode* qExprForCache = NULL;
  KjNode*  qP            = kjLookup(subP, "https://uri.etsi.org/ngsi-ld/q");
  if (qP != NULL && qP->type == KjString)
  {
    Tenant* tP = (Tenant*) swNgsild.tenantP;
    KAlloc* cacheAllocP = (tP->subCacheP != NULL) ? &((LdSubCache*) tP->subCacheP)->alloc : &swRest.kalloc;

    // Single parse — expands attr names via swNgsild.contextP, allocates with cache allocator
    qExprForCache = ldQParse(qP->value.s, cacheAllocP);
    if (qExprForCache != NULL)
    {
      // Render back to expanded q-string for DB storage
      char* expandedQ = ldQRender(qExprForCache, NULL, &swRest.kalloc);
      if (expandedQ != NULL)
        qP->value.s = expandedQ;
    }
  }

  //
  // Add "status" = "active"|"paused"|"expired" (read-only field, computed from isActive + expiresAt)
  //
  KjNode* isActiveP  = kjLookup(subP, LD_VOCAB_IS_ACTIVE);
  KjNode* expiresAtP = kjLookup(subP, LD_VOCAB_EXPIRES_AT);
  bool    isActive   = (isActiveP == NULL || isActiveP->type != KjBoolean || isActiveP->value.b == true);

  //
  // Per spec 5.8.1.4: expiresAt in the past is an error
  //
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

  KjNode* statusP = kjString(NULL, LD_VOCAB_STATUS, isActive ? "active" : "paused");
  kjChildAdd(subP, statusP);

  //
  // Store the request @context URL for notifications (jsonldContext per spec 5.2.13).
  // If no user context was provided, use the core context URL.
  //
  {
    SwldContext* reqCtxP = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();
    if (reqCtxP != NULL && reqCtxP->url != NULL)
    {
      KjNode* jcP = kjString(NULL, "jsonldContext", reqCtxP->url);
      kjChildAdd(subP, jcP);
    }
  }

  //
  // Create subscription in database
  //
  if (db.subscriptionCreate == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented", "subscription CRUD not supported by this DB plugin");
    return true;
  }

  int r = db.subscriptionCreate((Tenant*) swNgsild.tenantP, idP->value.s, subP);

  if (r == DB_ALREADY_EXISTS)
  {
    ldError(409, LD_ERROR_ALREADY_EXISTS, "Already Exists", "subscription '%s' already exists", idP->value.s);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error creating subscription '%s'", idP->value.s);
    return true;
  }

  //
  // Add to subscription cache
  //
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;
  //
  // mongocKjTreeToBson renames "id" to "_id" in-place — restore it.
  //
  if (idP->name[0] == '_')
    idP->name = "id";

  KjNode* timeIntervalP = kjLookup(subP, "timeInterval");
  bool isPernot = (timeIntervalP != NULL && (timeIntervalP->type == KjInt || timeIntervalP->type == KjFloat));

  if (isPernot)
  {
    if (tenantP->pernotCacheP != NULL)
      ldPernotCacheItemAdd((LdPernotCache*) tenantP->pernotCacheP, subP, qExprForCache, tenantP);
  }
  else
  {
    LdSubCacheItem* cachedP = NULL;
    if (tenantP->subCacheP != NULL)
      cachedP = ldSubCacheItemAdd((LdSubCache*) tenantP->subCacheP, subP, qExprForCache);

    //
    // § 5.8.1.4 — fan derived subs out to matching CSRs.
    // Skipped silently when --httpEndpoint is unset (ldBrokerHttpEndpoint == NULL),
    // when the tenant has no reg cache, or when --localOnly is in effect.
    //
    if (cachedP != NULL && tenantP->regCacheP != NULL && !ldLocalOnly)
    {
      const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);
      ldDistSubFanout(cachedP, (LdRegCache*) tenantP->regCacheP, ownAlias,
                      distSubPersist, tenantP);
    }
  }

  //
  // 201 Created -- set Location and Link headers, no body
  //
  swRest.out.httpStatusCode = 201;

  //
  // Location header: full path to the new subscription
  //
  const char* prefix  = "/ngsi-ld/v1/subscriptions/";
  int         locLen  = strlen(prefix) + strlen(idP->value.s) + 1;
  char*       locBuf  = kaAlloc(&swRest.kalloc, locLen);

  strcpy(locBuf, prefix);
  strcat(locBuf, idP->value.s);
  swRestOutHeaderAdd("Location", locBuf);

  SwldContext* ctxP   = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();
  const char*  ctxUrl = ctxP->url;

  if (ctxUrl != NULL)
  {
    const char* suffix  = ">; rel=\"http://www.w3.org/ns/json-ld#context\"; type=\"application/ld+json\"";
    int         linkLen = 1 + strlen(ctxUrl) + strlen(suffix) + 1;
    char*       linkBuf = kaAlloc(&swRest.kalloc, linkLen);

    strcpy(linkBuf, "<");
    strcat(linkBuf, ctxUrl);
    strcat(linkBuf, suffix);
    swRestOutHeaderAdd("Link", linkBuf);
  }

  return true;
}

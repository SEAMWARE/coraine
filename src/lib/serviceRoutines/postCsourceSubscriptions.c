//
// FILE            postCsourceSubscriptions.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// POST /ngsi-ld/v1/csourceSubscriptions  (NGSI-LD § 5.11.2)
//
// Creates a CSR-subscription in the per-tenant regSubCacheP. Uses the
// same Subscription data type as entity-subs but is routed to a
// separate cache instance so the entity-notify hot path never iterates
// registration-subs (and vice versa).
//
// V1 scope: cache-only, no DB persistence. timeInterval -> 501.
// Initial-on-subscribe notification is emitted from here (§ 5.11.2.4 /
// § 5.11.7): a Context Source Notification with all currently
// matching CSRs and triggerReason="newlyMatching" is POSTed to the
// subscription's endpoint.
//
#include <string.h>                                  // strlen, strcpy, strcat
#include <stdio.h>                                   // snprintf
#include <time.h>                                    // time

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/corRestOutHeader.h"                  // corRestOutHeaderAdd
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjBuilder.h"                         // kjString, kjChildAdd
#include "kjson/KjNode.h"                            // KjNode, KjString
#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "corJsonld/corLdInit.h"                       // corLdCoreContext
#include "corJsonld/CorLdContext.h"                    // CorLdContext
#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/ldCheckSubscription.h"            // ldCheckSubscription
#include "corNgsild/LdOp.h"                           // LdOpCreateCsourceSubscription
#include "corNgsild/LdVocab.h"                        // LD_VOCAB_IS_ACTIVE, LD_VOCAB_STATUS
#include "corNgsild/LdSubCache.h"                     // LdSubCache
#include "corNgsild/ldSubCache.h"                     // ldSubCacheItemAdd, ldSubCacheItemLookup
#include "corNgsild/ldSysTimestamp.h"                 // ldSysTimestampCreate
#include "corNgsild/LdRegCache.h"                     // LdRegCache
#include "corNgsild/ldCsrSubNotify.h"                 // ldCsrSubInitialNotify

#include "db/DbDriver.h"                             // db, DB_OK, DB_ALREADY_EXISTS
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postCsourceSubscriptions.h"  // Own interface



static char* csrSubIdGenerate(KAlloc* allocP)
{
  static int counter = 0;
  char*      buf     = kaAlloc(allocP, 128);

  snprintf(buf, 128, "urn:ngsi-ld:Subscription:%lx:%04x", (long) time(NULL), ++counter & 0xFFFF);

  return buf;
}



bool postCsourceSubscriptions(void)
{
  KjNode* subP = corRest.in.requestTree;

  LdFormat notifFormat = LdFormatNone;
  if (ldCheckSubscription(subP, LdOpCreateCsourceSubscription, /*merged*/false, &notifFormat, &corRest.kalloc) == false)
    return true;

  // § 5.11.7 — timeInterval drives periodic CsourceNotifications.
  // Validate it here; it gets transferred to the cache item further
  // down, after the cache add.
  int timeIntervalSec = 0;
  KjNode* tiP = kjLookup(subP, "timeInterval");
  if (tiP != NULL)
  {
    long n = (tiP->type == KjInt)   ? (long) tiP->value.i
           : (tiP->type == KjFloat) ? (long) tiP->value.f
           : -1L;
    if (n < 1)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Field Value",
              "'timeInterval' must be a positive integer (seconds)");
      return true;
    }
    timeIntervalSec = (int) n;
  }

  //
  // Extract or generate subscription id
  //
  KjNode* idP = kjLookup(subP, "id");

  if (idP == NULL)
  {
    char* generatedId = csrSubIdGenerate(&corRest.kalloc);

    idP = kjString(corRest.kjsonP, "id", generatedId);
    kjChildAdd(subP, idP);
  }
  else if (idP->type != KjString)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Field Value", "subscription 'id' must be a string");
    return true;
  }

  Tenant* tenantP = (Tenant*) corNgsild.tenantP;

  // Reject duplicate id in either sub cache — /subscriptions and
  // /csourceSubscriptions share the mongo collection, so the id space
  // is effectively global. Each existence check is a pure read of the
  // relevant cache — hold a brief rdlock of the RIGHT cache around it.
  {
    LdSubCache* regSubCacheP = (LdSubCache*) tenantP->regSubCacheP;
    ldSubCacheRdLock(regSubCacheP);
    bool exists = (regSubCacheP != NULL && ldSubCacheItemLookup(regSubCacheP, idP->value.s) != NULL);
    ldSubCacheUnlock(regSubCacheP);
    if (exists)
    {
      ldError(409, LD_ERROR_ALREADY_EXISTS, "Already Exists",
              "CSR subscription '%s' already exists", idP->value.s);
      return true;
    }
  }
  {
    LdSubCache* subCacheP = (LdSubCache*) tenantP->subCacheP;
    ldSubCacheRdLock(subCacheP);
    bool exists = (subCacheP != NULL && ldSubCacheItemLookup(subCacheP, idP->value.s) != NULL);
    ldSubCacheUnlock(subCacheP);
    if (exists)
    {
      ldError(409, LD_ERROR_ALREADY_EXISTS, "Already Exists",
              "subscription '%s' already exists", idP->value.s);
      return true;
    }
  }

  //
  // status = "active"|"paused" — expiresAt-past check
  //
  KjNode* isActiveP  = kjLookup(subP, LD_VOCAB_IS_ACTIVE);
  bool    isActive   = (isActiveP == NULL || isActiveP->type != KjBoolean || isActiveP->value.b == true);

  KjNode* statusP = kjString(corRest.kjsonP, LD_VOCAB_STATUS, isActive ? "active" : "paused");
  kjChildAdd(subP, statusP);

  // § 6.4.5 — system-generated createdAt/modifiedAt (nanosecond integers in
  // the persisted tree; rendered to ISO only when the client asks for sysAttrs)
  ldSysTimestampCreate(subP);

  //
  // jsonldContext: only auto-fill when the user didn't supply one. The
  // auto-filled URL goes into the broker-internal `_jcResolved` (same
  // convention as regular subs) so retrieve doesn't surface a synthetic
  // value. Mirror the three shapes postSubscriptions handles:
  //   1. single-URL context           → use the URL directly
  //   2. array-of-one URL in the body → use that URL directly
  //   3. anything else (multi-URL or inline object) → fall back to the
  //      compound's own URL, otherwise the core context
  //
  if (kjLookup(subP, "jsonldContext") == NULL)
  {
    const char*  jcUrl   = NULL;
    CorLdContext* reqCtxP = (corNgsild.contextP != NULL) ? corNgsild.contextP : corLdCoreContext();

    if (reqCtxP != NULL && reqCtxP->url != NULL && !reqCtxP->isArray)
      jcUrl = reqCtxP->url;
    else if (corNgsild.userContextBody != NULL &&
             corNgsild.userContextBody->type == KjArray &&
             corNgsild.userContextBody->value.firstChildP != NULL &&
             corNgsild.userContextBody->value.firstChildP->next == NULL &&
             corNgsild.userContextBody->value.firstChildP->type == KjString)
      jcUrl = corNgsild.userContextBody->value.firstChildP->value.s;
    else if (reqCtxP != NULL && reqCtxP->url != NULL)
      jcUrl = reqCtxP->url;

    if (jcUrl != NULL)
    {
      KjNode* jcP = kjString(corRest.kjsonP, "_jcResolved", jcUrl);
      kjChildAdd(subP, jcP);
    }
  }

  //
  // Internal marker: distinguishes CSR-subs from entity-subs in the
  // shared mongo collection. Stripped from GET responses.
  //
  kjChildAdd(subP, kjString(corRest.kjsonP, "_subKind", "csr"));

  //
  // Persist (same collection as entity subs). If no DB plugin is
  // configured, we still add to cache — CSR-subs remain in-memory
  // and are lost on restart.
  //
  if (db.subscriptionCreate != NULL)
  {
    int r = db.subscriptionCreate(tenantP, idP->value.s, subP);

    if (r == DB_ALREADY_EXISTS)
    {
      ldError(409, LD_ERROR_ALREADY_EXISTS, "Already Exists",
              "subscription '%s' already exists", idP->value.s);
      return true;
    }
    if (r != DB_OK)
    {
      ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
              "database error creating CSR subscription '%s'", idP->value.s);
      return true;
    }

    // mongocKjTreeToBson renames "id" to "_id" in-place — restore it.
    if (idP->name[0] == '_')
      idP->name = "id";
  }

  //
  // Add to CSR-subscription cache under the wrlock; pin the new item and
  // drop the lock before the reg-touching initial notify (lock order is
  // reg-before-sub, so the sub lock must not be held across it).
  //
  LdSubCache*     regSubCacheP = (LdSubCache*) tenantP->regSubCacheP;
  LdSubCacheItem* cacheItem    = NULL;
  ldSubCacheWrLock(regSubCacheP);
  if (regSubCacheP != NULL)
  {
    cacheItem = ldSubCacheItemAdd(regSubCacheP, subP, NULL, notifFormat);
    if (cacheItem != NULL)
    {
      cacheItem->timeInterval = timeIntervalSec;
      ldSubCacheItemPin(cacheItem);
    }
  }
  ldSubCacheUnlock(regSubCacheP);

  //
  // 201 Created — Location + Link headers
  //
  corRest.out.httpStatusCode = 201;

  const char* prefix  = "/ngsi-ld/v1/csourceSubscriptions/";
  int         locLen  = strlen(prefix) + strlen(idP->value.s) + 1;
  char*       locBuf  = kaAlloc(&corRest.kalloc, locLen);

  strcpy(locBuf, prefix);
  strcat(locBuf, idP->value.s);
  corRestOutHeaderAdd("Location", locBuf);

  // § 6.3.6: no Link header on no-body responses.

  //
  // Initial-on-subscribe notification (§ 5.11.2.4 / § 5.11.7): walk the
  // reg cache, find CSRs matching this sub per § 5.12, and fire a single
  // CsourceNotification with triggerReason="newlyMatching". No-op when
  // no CSRs match. The 201 has already been committed to the client;
  // a notification-side failure must not flip the create result.
  //
  if (cacheItem != NULL && tenantP->regCacheP != NULL)
    ldCsrSubInitialNotify((LdRegCache*) tenantP->regCacheP, cacheItem);
  if (cacheItem != NULL)
    ldSubCacheItemUnpin(cacheItem);

  return true;
}

//
// FILE            postSubscriptions.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <string.h>                                  // strlen, strcpy, strcat
#include <stdio.h>                                   // snprintf
#include <time.h>                                    // time

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/corRestOutHeader.h"                  // corRestOutHeaderAdd
#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corTree/corTreeClone.h"                    // corTreeClone
#include "corTree/corTreeBuilder.h"                  // corTreeString, corTreeChildAdd
#include "corTree/CorNode.h"                         // CorNode, CorString
#include "corAlloc/corAlloc.h"                       // corAlloc
#include "corAlloc/CorAlloc.h"                         // CorAlloc
#include "corJsonld/corLdInit.h"                       // corLdCoreContext
#include "corJsonld/CorLdContext.h"                    // CorLdContext
#include "corJsonld/CorLdContextCache.h"               // CorLdContextCache
#include "corJsonld/corLdCache.h"                      // corLdCacheInsert
#include "corJsonld/corLdContextParse.h"               // corLdContextFromObject, corLdContextFromTree
#include "corJsonld/corLdIdGen.h"                      // corLdIdGenerate
#include "corJson/corJsonRender.h"                   // corJsonFastRender
#include "corJson/corJsonRenderSize.h"               // corJsonFastRenderSize

extern CorLdContextCache* corLdCacheGet(void);
#include "corNgsild/CorNgsild.h"                     // ldBrokerHttpEndpoint, corNgsild
#include "db/DbDriver.h"                             // db, DB_CONTEXT_KIND_IMPLICIT
#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/ldCheckSubscription.h"            // ldCheckSubscription
#include "corNgsild/LdOp.h"                           // LdOpCreateSubscription
#include "corNgsild/LdVocab.h"                        // LD_VOCAB_IS_ACTIVE, LD_VOCAB_STATUS
#include "corNgsild/LdSubCache.h"                     // LdSubCache
#include "corNgsild/ldSubCache.h"                     // ldSubCacheItemAdd
#include "corNgsild/ldSysTimestamp.h"                 // ldSysTimestampCreate
#include "corNgsild/ldPernotCache.h"                  // ldPernotCacheItemAdd
#include "corNgsild/LdRegCache.h"                     // LdRegCache
#include "corNgsild/ldDistSub.h"                      // ldDistSubFanout
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "corNgsild/ldTermId.h"                          // ldNodeRename

#include "corJsonld/corLdDownload.h"                   // corLdContextFromUrl

#include "db/DbDriver.h"                             // db, DB_OK, DB_ALREADY_EXISTS
#include "db/Tenant.h"                               // Tenant

#include "corNgsild/ldIdGenerate.h"                  // ldIdGenerate
#include "serviceRoutines/subscriptionQExpand.h"   // subscriptionQExpand
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
  CorNode* fragP = ldDistSubSubordinatesFragment(itemP, corRest.kallocP);
  if (fragP == NULL)
    return;

  db.subscriptionUpdate(tP, itemP->subId, fragP);
}



// -----------------------------------------------------------------------------
//
// subIdGenerate - generate a subscription id if none provided
//
static char* subIdGenerate(CorAlloc* allocP)
{
  return ldIdGenerate(allocP, "Subscription");   // shared, atomic counter - see corNgsild ldIdGenerate.c
}



// -----------------------------------------------------------------------------
//
// postSubscriptions -
//
bool postSubscriptions(void)
{
  CorNode* subP = corRest.in.requestTree;

  //
  // Must have a JSON payload
  //
  //
  // Validate the subscription. The notification format is parsed here and
  // carried to ldSubCacheItemAdd below, so the string is matched only once.
  //
  LdFormat notifFormat = LdFormatNone;
  if (ldCheckSubscription(subP, LdOpCreateSubscription, /*merged*/false, &notifFormat, &corRest.kalloc) == false)
    return true;

  //
  // Extract or generate subscription id
  //
  CorNode* idP = corTreeLookup(subP, "id");

  if (idP == NULL)
  {
    char* generatedId = subIdGenerate(&corRest.kalloc);

    idP = corTreeString(corRest.kallocP, "id", generatedId);
    corTreeChildAdd(subP, idP);
  }
  else if (idP->type != CorString)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Field Value", "subscription 'id' must be a string");
    return true;
  }

  //
  // Reject id collision with an existing CSR-subscription — the mongo
  // collection is shared across /subscriptions and /csourceSubscriptions.
  //
  {
    Tenant*     _t           = (Tenant*) corNgsild.tenantP;
    LdSubCache* regSubCacheP = (_t != NULL) ? (LdSubCache*) _t->regSubCacheP : NULL;

    // Under the CSR-sub cache's rdlock (as postCsourceSubscriptions does) - it had none
    ldSubCacheRdLock(regSubCacheP);
    bool exists = (regSubCacheP != NULL) && (ldSubCacheItemLookup(regSubCacheP, idP->value.s) != NULL);
    ldSubCacheUnlock(regSubCacheP);

    if (exists)
    {
      ldError(409, LD_ERROR_ALREADY_EXISTS, "Already Exists",
              "subscription '%s' already exists", idP->value.s);
      return true;
    }
  }

  //
  // q is stored with its attribute names EXPANDED (subscriptionQExpand), and each cache parses the
  // stored q itself, into memory it owns. This used to parse it into the subscription cache's
  // shared arena - outside its lock, so two POSTs at once raced on it - meaning to store it
  // expanded too; but it looked q up by an IRI the tree does not carry, so that never ran, and
  // the expanded form it would have stored was one ldQParse could not read back.
  //
  subscriptionQExpand(subP);

  //
  // Add "status" = "active"|"paused"|"expired" (read-only field, computed from isActive + expiresAt)
  //
  CorNode* isActiveP = corTreeLookup(subP, LD_VOCAB_IS_ACTIVE);
  CorNode* expiresAtP = corTreeLookup(subP, LD_VOCAB_EXPIRES_AT);
  bool    isActive   = (isActiveP == NULL || isActiveP->type != CorBoolean || isActiveP->value.b == true);

  // § 5.2.12 — `isActive` is cardinality 0..1 and "true by default". When
  // the user creates a subscription WITHOUT isActive, do not synthesize a
  // default value into the stored subTree: the active/paused state is
  // already observable through `status`, and emitting isActive=true here
  // would surface it in retrieve responses even though the user didn't
  // ask for it (ETSI 028_06). The user's explicit value (e.g.
  // `isActive: false`) is preserved unchanged.

  //
  // Per spec 5.8.1.4: expiresAt in the past is an error
  //
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

  CorNode* statusP = corTreeString(corRest.kallocP, LD_VOCAB_STATUS, isActive ? "active" : "paused");
  corTreeChildAdd(subP, statusP);

  // § 6.4.5 — system-generated createdAt/modifiedAt (nanosecond integers in
  // the persisted tree; rendered to ISO only when the client asks for sysAttrs)
  ldSysTimestampCreate(subP);

  //
  // § 5.5.5 / § 5.8.1.4 — auto-populate `jsonldContext` when the user
  // didn't supply one. Three flavours, in priority order:
  //
  //   1. user already gave jsonldContext             → keep as-is
  //   2. @context is a single URL string             → use that URL
  //   3. @context is an array or inline-object form  → mint an
  //      ImplicitlyCreated @context entry, persist it, use its served URL
  //
  // Without this, a notification fired by this subscription would carry
  // a Link header (§ 6.3.19) pointing at a context the receiver can't
  // dereference (the inline body has no URL of its own).
  //
  //
  // § 5.8.5 — when the user supplies `jsonldContext`, validate it
  // is dereferenceable up front. If the URL can't be fetched, the
  // notification renderer will fail later with no good way to
  // report it; surface the failure now as 504 (matches
  // /jsonldContexts create + 043_01_05 CSR-create behaviour).
  //
  CorNode* userJcP = corTreeLookup(subP, "jsonldContext");
  if (userJcP != NULL && userJcP->type == CorString)
  {
    if (corLdContextFromUrl(userJcP->value.s, &corRest.kalloc) == NULL)
    {
      ldError(504, LD_ERROR_LD_CONTEXT_NOT_AVAILABLE, "Context Not Available",
              "subscription jsonldContext '%s' could not be fetched", userJcP->value.s);
      return true;
    }
  }

  if (corTreeLookup(subP, "jsonldContext") == NULL)
  {
    const char* jcUrl = NULL;

    CorLdContext* reqCtxP = corNgsild.contextP;
    if (reqCtxP != NULL && reqCtxP->url != NULL && !reqCtxP->isArray)
    {
      jcUrl = reqCtxP->url;
    }
    // An @context body that's an array of a single string URL is
    // semantically the same as just that URL — use it directly instead
    // of minting an Implicit. ETSI 046_10 / 046_11 supply their @context
    // this way, and a notification's Link header is expected to point
    // at the user's URL, not at a broker-minted alias.
    else if (corNgsild.userContextBody != NULL &&
             corNgsild.userContextBody->type == CorArray &&
             corNgsild.userContextBody->value.head != NULL &&
             corNgsild.userContextBody->value.head->next == NULL &&
             corNgsild.userContextBody->value.head->type == CorString)
    {
      jcUrl = corNgsild.userContextBody->value.head->value.s;
    }
    else if (corNgsild.userContextBody != NULL)
    {
      // Mint an Implicit entry from the inline body. The same plumbing
      // that POST /jsonldContexts uses (id generation, body persistence,
      // cache insert), but with kind=ImplicitlyCreated since the broker
      // is the originator, not a client request. Per § 5.13 the
      // resulting URL is the SPEC-visible `jsonldContext` on the
      // subscription (clients use it to dereference the minted
      // @context) — not the internal `_jcResolved` alias used by the
      // other auto-fill branches.
      CorLdContextCache* cacheP = corLdCacheGet();
      CorAlloc*         storeP = (cacheP != NULL) ? cacheP->kaP : &corRest.kalloc;
      char* implicitId = corLdIdGenerate(storeP);
      if (implicitId != NULL)
      {
        CorLdContext* implicitP = NULL;
        if (corNgsild.userContextBody->type == CorObject)
          implicitP = corLdContextFromObject(corNgsild.userContextBody, storeP, NULL);
        else
          implicitP = corLdContextFromTree(corNgsild.userContextBody, storeP, NULL);  // @context from the request - no URL of its own

        int   bodyLen = corJsonFastRenderSize(corNgsild.userContextBody) + 32;
        char* bodyBuf = (char*) corAlloc(storeP, bodyLen);
        if (bodyBuf != NULL && implicitP != NULL)
        {
          // The body we persist is the wrapper {"@context": <orig>} so
          // GET /jsonldContexts/{id} returns a self-contained document.
          int p = 0;
          p += snprintf(bodyBuf + p, bodyLen - p, "{\"@context\":");
          corJsonFastRender(corNgsild.userContextBody, bodyBuf + p);
          p += strlen(bodyBuf + p);
          p += snprintf(bodyBuf + p, bodyLen - p, "}");
          implicitP->body = bodyBuf;
          implicitP->id   = implicitId;
          implicitP->kind = CorLdKindImplicit;
          corLdCacheInsert(implicitP);
          if (db.contextSave != NULL)
            db.contextSave(implicitId, NULL, DB_CONTEXT_KIND_IMPLICIT, bodyBuf);
        }

        // jsonldContext URL = served URL = <httpEndpoint>/ngsi-ld/v1/jsonldContexts/{id}
        const char* prefix = "/ngsi-ld/v1/jsonldContexts/";
        const char* base   = (ldBrokerHttpEndpoint != NULL) ? ldBrokerHttpEndpoint : "";
        int   baseLen      = strlen(base);
        int   prefixLen    = strlen(prefix);
        int   idLen        = strlen(implicitId);
        char* urlBuf       = (char*) corAlloc(&corRest.kalloc, baseLen + prefixLen + idLen + 1);
        memcpy(urlBuf, base, baseLen);
        memcpy(urlBuf + baseLen, prefix, prefixLen);
        memcpy(urlBuf + baseLen + prefixLen, implicitId, idLen);
        urlBuf[baseLen + prefixLen + idLen] = 0;
        jcUrl = urlBuf;
      }
    }
    else
    {
      // No user context (or only the core was used). Default to the
      // configured core URL so notifications still carry a workable Link.
      CorLdContext* coreP = corLdCoreContext();
      if (coreP != NULL && coreP->url != NULL)
        jcUrl = coreP->url;
    }

    //
    // The URL is spec-visible in EVERY branch, under its spec name.
    //
    // § 10.5.2.4: "If not present, the jsonldContext field shall be
    // initialized with the @context applicable for the Subscription." Not
    // "may", and not "if the broker had to mint one" - a client that reads a
    // subscription back is entitled to see which @context its notifications
    // will carry.
    //
    // Only the minted-Implicit branch used to write the spec name; the
    // passed-through and core-default branches wrote the broker-internal
    // `_jcResolved`, which every retrieve path then stripped. The value was
    // right and simply invisible, which is why this reads as a missing
    // feature and was in fact a naming bug.
    //
    if (jcUrl != NULL)
      corTreeChildAdd(subP, corTreeString(corRest.kallocP, "jsonldContext", (char*) jcUrl));
  }

  //
  // Create subscription in database
  //
  if (db.subscriptionCreate == NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented", "subscription CRUD not supported by this DB plugin");
    return true;
  }

  int r = db.subscriptionCreate((Tenant*) corNgsild.tenantP, idP->value.s, subP);

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
  Tenant* tenantP = (Tenant*) corNgsild.tenantP;
  //
  // mongocTreeToBson renames "id" to "_id" in-place — restore it.
  //
  if (idP->name[0] == '_')
    ldNodeRename(idP, "id");

  CorNode* timeIntervalP = corTreeLookup(subP, "timeInterval");
  bool isPernot = (timeIntervalP != NULL && (timeIntervalP->type == CorInt || timeIntervalP->type == CorFloat));

  if (isPernot)
  {
    if (tenantP->pernotCacheP != NULL)
      ldPernotCacheItemAdd((LdPernotCache*) tenantP->pernotCacheP, subP, tenantP);
  }
  else
  {
    // Add under the sub wrlock; pin the new item and drop the lock before the
    // fanout (it reads the reg cache + forwards derived subs over the network —
    // lock order is reg-before-sub, so we must not hold the sub lock across it).
    LdSubCache*     subCacheP = (LdSubCache*) tenantP->subCacheP;
    LdSubCacheItem* cachedP   = NULL;
    ldSubCacheWrLock(subCacheP);
    if (subCacheP != NULL)
    {
      cachedP = ldSubCacheItemAdd(subCacheP, subP, NULL, notifFormat);   // parses the stored q
      if (cachedP != NULL)
        ldSubCacheItemPin(cachedP);
    }
    ldSubCacheUnlock(subCacheP);

    //
    // § 5.8.1.4 — fan derived subs out to matching CSRs.
    // Skipped silently when --httpEndpoint is unset (ldBrokerHttpEndpoint == NULL),
    // when the tenant has no reg cache, or with --distributed off.
    //
    if (cachedP != NULL && tenantP->regCacheP != NULL && ldDistributed)
    {
      const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);
      ldDistSubFanout(cachedP, (LdRegCache*) tenantP->regCacheP, ownAlias,
                      distSubPersist, tenantP);
    }
    if (cachedP != NULL)
      ldSubCacheItemUnpin(cachedP);
  }

  //
  // 201 Created -- set Location and Link headers, no body
  //
  corRest.out.httpStatusCode = 201;

  //
  // Location header: full path to the new subscription
  //
  const char* prefix  = "/ngsi-ld/v1/subscriptions/";
  int         locLen  = strlen(prefix) + strlen(idP->value.s) + 1;
  char*       locBuf  = corAlloc(&corRest.kalloc, locLen);

  strcpy(locBuf, prefix);
  strcat(locBuf, idP->value.s);
  corRestOutHeaderAdd("Location", locBuf);

  // § 6.3.6: no Link header on no-body responses.

  return true;
}

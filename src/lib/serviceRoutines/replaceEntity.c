//
// FILE            replaceEntity.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// PUT /ngsi-ld/v1/entities/{entityId} — Replace Entity.
// NGSI-LD v1.9.1 § 5.6.18 / § 5.5.12.
//

#include <stddef.h>                                   // NULL
#include <string.h>                                   // strcmp, strlen, strcpy
#include <stdlib.h>                                   // free
#include <stdio.h>                                    // snprintf

#include "swRest/SwRestState.h"                       // swRest
#include "swRest/SwRestVerb.h"                        // SwVerbPut

#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/kjBuilder.h"                          // kjObject, kjArray, kjString, kjChildAdd
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjRender.h"                           // kjFastRender
#include "kjson/kjRenderSize.h"                       // kjFastRenderSize

#include "kalloc/kaAlloc.h"                           // kaAlloc

#include "swJsonld/swldInit.h"                        // swldCoreContext, SWLD_CORE_CONTEXT_URL
#include "swJsonld/swldCompactTree.h"                 // swldCompactTreeWith

#include "swNgsild/swNgsild.h"                        // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldCheckEntity.h"                   // ldCheckEntity
#include "swNgsild/LdOp.h"                            // LdOpReplaceEntity
#include "swNgsild/ldApiEntityToDbModel.h"            // ldApiEntityToDbModel
#include "swNgsild/LdProblem.h"                       // LD_ERROR_CONFLICT
#include "swNgsild/LdSubCache.h"                      // LdSubCache
#include "swNgsild/ldSubscriptionNotify.h"            // LdNotifyEntityUpdate
#include "swNgsild/ldNotifyDefer.h"                   // ldNotifyDefer

#include "troe/TroeDriver.h"                          // TroeEvent, TroeOp*
#include "troe/troeDispatch.h"                        // troeDeferEntityEvent, troeDeferAttrEvent

#include "swNgsild/LdRegCache.h"                      // LdRegCache, LdRegCacheItem, LdRegMode, LdRegInfo
#include "swNgsild/ldRegCache.h"                      // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "swNgsild/ldCsourceAlias.h"                  // ldCsourceAliasForTenant
#include "swNgsild/ldDistOp.h"                        // ldDistOpLoopDetected, ldDistOpSend, ldDistOpBatchErrorAdd
#include "swNgsild/ldEntityFragment.h"                // ldEntityFragmentForInfo

#include "db/DbDriver.h"                              // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                                // Tenant

#include "serviceRoutines/replaceEntity.h"            // Own interface



// -----------------------------------------------------------------------------
//
// typeEqual - compare two NGSI-LD entity "type" nodes for equality.
//
// Both must be the same shape (string vs. array) and carry the same set of
// values. String comparison is exact (the values are expanded IRIs).
//
static bool typeEqual(KjNode* a, KjNode* b)
{
  if ((a == NULL) || (b == NULL))
    return false;

  if ((a->type == KjString) && (b->type == KjString))
    return strcmp(a->value.s, b->value.s) == 0;

  if ((a->type != KjArray) || (b->type != KjArray))
    return false;

  for (KjNode* aI = a->value.firstChildP; aI != NULL; aI = aI->next)
  {
    if (aI->type != KjString)
      return false;

    bool found = false;
    for (KjNode* bI = b->value.firstChildP; bI != NULL; bI = bI->next)
    {
      if ((bI->type == KjString) && (strcmp(aI->value.s, bI->value.s) == 0))
      {
        found = true;
        break;
      }
    }
    if (!found)
      return false;
  }

  for (KjNode* bI = b->value.firstChildP; bI != NULL; bI = bI->next)
  {
    if (bI->type != KjString)
      return false;

    bool found = false;
    for (KjNode* aI = a->value.firstChildP; aI != NULL; aI = aI->next)
    {
      if ((aI->type == KjString) && (strcmp(bI->value.s, aI->value.s) == 0))
      {
        found = true;
        break;
      }
    }
    if (!found)
      return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// replaceUrl - compose the PUT forward URL for one CSR
//
// <endpoint>/ngsi-ld/v1/entities/<entityId>
//
static char* replaceUrl(const char* endpoint, const char* entityId)
{
  const char* path    = "/ngsi-ld/v1/entities/";
  int         baseLen = strlen(endpoint);
  int         pathLen = strlen(path);
  int         idLen   = strlen(entityId);
  char*       url     = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + idLen + 1);
  strcpy(url, endpoint);
  strcpy(url + baseLen, path);
  strcpy(url + baseLen + pathLen, entityId);
  return url;
}



// -----------------------------------------------------------------------------
//
// renderFragmentWithContext - serialize fragment with @context for remote
//
static char* renderFragmentWithContext(KjNode* fragP)
{
  // Strip body @context: forward goes out as application/json + Link.
  KjNode* atCtx = kjLookup(fragP, "@context");
  if (atCtx != NULL)
    kjChildRemove(fragP, atCtx);

  int   bufSize = kjFastRenderSize(fragP) + 1;
  char* buf     = (char*) kaAlloc(&swRest.kalloc, bufSize);

  kjFastRender(fragP, buf);
  return buf;
}



// -----------------------------------------------------------------------------
//
// forwardReplaceEntity - PUT entity fragment to a CSR endpoint
//
static int forwardReplaceEntity(LdRegCacheItem* csr,
                                KjNode*         fragP,
                                const char*     entityId,
                                const char*     ownAlias,
                                const char**    errorDetailPP)
{
  char* body = renderFragmentWithContext(fragP);
  return ldDistOpSend(csr, SwVerbPut,
                      replaceUrl(csr->endpoint, entityId),
                      body, strlen(body), ownAlias, errorDetailPP);
}



// -----------------------------------------------------------------------------
//
// replaceEntity -
//
bool replaceEntity(void)
{
  const char* entityId = swRest.in.wildcard[0];
  KjNode*     entityP  = swRest.in.requestTree;

  //
  // Validate payload as a full entity (id + type mandatory, no null-marker).
  //
  if (ldCheckEntity(entityP, LdOpReplaceEntity, NULL, &swRest.kalloc) == false)
    return true;

  //
  // Id consistency: body id (if present) must match URL id
  //
  KjNode* bodyIdP = kjLookup(entityP, "id");
  if (bodyIdP != NULL && bodyIdP->type == KjString && strcmp(bodyIdP->value.s, entityId) != 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Entity Id Mismatch",
            "entity id in payload ('%s') does not match URL ('%s')",
            bodyIdP->value.s, entityId);
    return true;
  }

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;
  //
  // § 9.3.3 guard — a ?local=true write must not produce local data that an
  // exclusive or redirect registration claims.
  //
  if (swNgsild.local == true && tenantP->regCacheP != NULL)
  {
    const char* cRegId = ldRegCacheLocalWriteConflictTree((LdRegCache*) tenantP->regCacheP,
                                                          entityId, entityP, &swRest.kalloc);
    if (cRegId != NULL)
    {
      ldError(409, LD_ERROR_ALREADY_EXISTS, "Conflict",
              "local update overlaps with registration '%s' (§ 9.3.3 — no local data for an exclusive/redirect scope)",
              cRegId);
      return true;
    }
  }


  //
  // Pre-retrieve local entity — used both to enforce the "type shall not
  // change" guard (§ 5.6.18 local procedure) and to detect whether the
  // local path will run at all (if local entity doesn't exist, we skip
  // the local replace and rely on forwards succeeding).
  //
  KjNode* oldStored   = NULL;
  int     rr          = db.entityRetrieve(tenantP, entityId, &oldStored);
  bool    localExists = (rr == DB_OK && oldStored != NULL);

  if (rr != DB_OK && rr != DB_NOT_FOUND)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "database error retrieving entity '%s'", entityId);
    return true;
  }

  //
  // Type-change guard: if a local entity exists, its stored type must match
  // the body's type (Replace Entity must not alter type — swBroker policy,
  // stricter than spec's local "completely replace"). Decide this BEFORE
  // any forward: a partial success where local refuses for type reasons
  // would be hard to express in a BatchOperationResult.
  //
  if (localExists)
  {
    KjNode* newTypeP = kjLookup(entityP, "type");
    KjNode* oldTypeP = kjLookup(oldStored, "type");

    if (!typeEqual(newTypeP, oldTypeP))
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Immutable Field",
              "entity type cannot be changed on Replace");
      return true;
    }
  }

  //
  // Dispatch — § 5.6.18.4 Replace Entity. Skipped when:
  //   - ?local=true;
  //   - no registration cache on tenant;
  //   - incoming Via already carries our own alias (loop — skip forwards,
  //     still run local replace).
  //
  // Per § 5.6.18.4: "Attributes from matching input data are forwarded"
  // for each CSR. ldEntityFragmentForInfo extracts only the attrs from
  // entityP that the CSR's RegistrationInfo claims — id/type/@context
  // are cloned along so the forwarded body is a valid Replace Entity
  // input. Exclusive/redirect DETACH (chop); inclusive CLONE (keep
  // local copy for the post-dispatch remains).
  //
  KjNode* errorsArrayP = kjArray(swRest.kjsonP, "errors");
  bool    anySucceeded = false;

  // § 6.3.5 single-source error contract — see patchEntity for the full rationale.
  bool    singleAuthoritative = false;   // exactly one exclusive/redirect source, no inclusive
  int     forwardFailCount    = 0;       // forwarded entries that failed (non-2xx, non-404)
  bool    forwardTimedOut     = false;   // that failed forward was a broker per-CSR timeout

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

  bool dispatch = (swNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  // Loops handled in the dispatch block (builder marks loop-blocked CSRs; the
  // chop loop turns excl/redirect ones into 508 per § 6.3.18).

  if (dispatch)
  {
    KjNode* typeP = kjLookup(entityP, "type");
    char*   typeArr[2] = { NULL, NULL };
    char**  typeArgP   = NULL;
    if (typeP != NULL && typeP->type == KjString)
    {
      typeArr[0] = typeP->value.s;
      typeArgP   = typeArr;
    }

    LdRegCacheItem** exclV  = NULL;
    LdRegCacheItem** redirV = NULL;
    LdRegCacheItem** inclV  = NULL;
    int exclN  = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  entityId, typeArgP, NULL,
                                                  LdRegModeExclusive, &exclV);
    int redirN = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  entityId, typeArgP, NULL,
                                                  LdRegModeRedirect, &redirV);
    int inclN  = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  entityId, typeArgP, NULL,
                                                  LdRegModeInclusive, &inclV);

    singleAuthoritative = ((exclN + redirN) == 1) && (inclN == 0);

    //
    // Attribute chopping per mode (§ 4.3.6.3 / § 10.2.10.4):
    //   * Exclusive: each CSR owns its claimed attrs uniquely, so detach as we
    //     go — no two exclusives claim the same attribute.
    //   * Redirect: multiple redirect CSRs covering the same entity are meant
    //     to ALL receive the replace (§ 9.3.3 "multiple distinct redirect
    //     registrations can apply at the same time") — clone for each forward
    //     and do a single detach sweep after the last redirect. (D007_01_red
    //     regressed when redirect chopped in-loop: the second CSR saw an empty
    //     fragment and only one of two PUTs was sent.)
    //   * Inclusive: clone — the local replace below still applies these attrs.
    //
    LdDistOpGroup groups[] = {
      { exclV,  exclN,  "exclusive", true  },
      { redirV, redirN, "redirect",  true  },
      { inclV,  inclN,  "inclusive", false },
    };

    LdDistOpEntry* items;
    int n = ldDistOpEntriesBuild(groups, 3, ownAlias,
                                  swRest.serviceP->ldOp, "replaceEntity",
                                  entityId, /*perRi=*/true, entityId, NULL,
                                  errorsArrayP, &items);

    int kept = 0;
    for (int i = 0; i < n; i++)
    {
      bool isExclusive = (items[i].modeIdx == 0);

      KjNode* fragP = ldEntityFragmentForInfo(entityP, items[i].riP, swRest.kjsonP,
                                              /*detach=*/isExclusive);
      if (fragP == NULL) continue;

      // Loop-blocked forward (§ 6.3.18): the claimed attrs are chopped from
      // entityP (exclusive in-loop, redirect post-loop) so they won't be
      // replaced locally — for excl/redirect that's 508; inclusive keeps its
      // clone and the local replace serves it, so just drop the forward.
      if (items[i].wouldLoop)
      {
        // Excl/redirect attrs are chopped (won't be replaced locally); flag so
        // a fully-loop-blocked replace flattens its terminal 404 to 508 (§ 6.3.18).
        if (items[i].errorMode)
          swNgsild.loopBlocked508 = true;
        continue;
      }

      // fragP is private (detached or cloned) — compact in place with the
      // per-CSR forward context before rendering the wire body.
      swldCompactTreeWith(fragP, ldDistOpForwardContext(items[i].csr));

      char* body = renderFragmentWithContext(fragP);
      items[kept] = items[i];
      items[kept].url     = replaceUrl(items[i].csr->endpoint, entityId);
      items[kept].body    = body;
      items[kept].bodyLen = strlen(body);
      kept++;
    }

    // Post-loop redirect-detach: now that every redirect leg has its clone,
    // strip from the original entity every attribute the redirect legs picked
    // up so the local replace below won't re-store them (redirect data lives
    // on the CSR, not locally).
    for (int i = 0; i < n; i++)
    {
      if (items[i].modeIdx != 1) continue;  // redirect only
      KjNode* drop = ldEntityFragmentForInfo(entityP, items[i].riP, swRest.kjsonP,
                                              /*detach=*/true);
      (void) drop;  // freed with the arena
    }

    ldDistOpEntriesPerform(items, kept, SwVerbPut, ownAlias);

    for (int i = 0; i < kept; i++)
    {
      int sc = items[i].statusCode;
      if (sc >= 200 && sc < 300)
        anySucceeded = true;
      else if (sc != 404)
      {
        bool to = items[i].timedOut;   // § 6.3.5: honest per-source 504 on timeout
        ldDistOpBatchErrorAdd(errorsArrayP, entityId, to ? 504 : ((sc >= 400) ? sc : 502),
                              LD_ERROR_INTERNAL_ERROR, to ? "Gateway Timeout" : "Bad Gateway",
                              ldDistOpForwardFailureReason(sc, items[i].errorDetail),
                              items[i].csr->regId);
        forwardFailCount++;
        if (to) forwardTimedOut = true;
      }
    }

    ldRegCacheMatchRelease(exclV,  exclN);
    ldRegCacheMatchRelease(redirV, redirN);
    ldRegCacheMatchRelease(inclV,  inclN);
  }

  //
  // Local replace — § 5.6.18.4: "If the target Entity exists locally,
  // completely replace the existing Entity with the same Entity ID with
  // the new Entity content provided." "New Entity content" here means
  // the body AFTER exclusive/redirect chopping — so the local entity
  // ends up with only the unclaimed attrs.
  //
  // When local doesn't exist but forwards succeeded, we silently skip —
  // the entity is served by the CSRs, not locally. The response
  // decision below still reports 204/207 based on anySucceeded.
  //
  if (localExists && db.entityReplace == NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
            "Replace Entity not supported by this DB plugin");
    return true;
  }

  if (localExists)
  {
    // § 6.5.3.3 — a Replace MUST preserve the entity's createdAt (set when the
    // entity first entered the system); only modifiedAt is bumped. Hand the
    // stored createdAt to ldApiEntityToDbModel so it stamps that instead of
    // 'now'. oldStored is non-NULL here (localExists implies it).
    KjNode*  oldCreatedAt  = kjLookup(oldStored, "createdAt");
    int64_t  keepCreatedAt = (oldCreatedAt != NULL && oldCreatedAt->type == KjInt) ? oldCreatedAt->value.i : 0;
    ldApiEntityToDbModel(entityP, &swRest.kalloc, keepCreatedAt);

    KjNode* replacedOld = NULL;
    int     r           = db.entityReplace(tenantP, entityId, entityP, &replacedOld);

    if (r == DB_OK)
    {
      anySucceeded = true;

      // mongoc's entityReplace renames "id" to "_id" in-place. Restore.
      if (bodyIdP != NULL && bodyIdP->name[0] == '_')
        bodyIdP->name = "id";

      if (tenantP->subCacheP != NULL)
      {
        // A Replace creates/updates/deletes attributes relative to the stored
        // entity. Build the per-attribute change-report (oldStored vs entityP,
        // both DB-model form) so attribute-level and default notificationTrigger
        // subscriptions fire — not just entityUpdated. § 5.2 notificationTrigger.
        LdMergeReport replaceReport;
        replaceReport.changes = NULL;
        ldEntityReplaceReport(oldStored, entityP, &replaceReport);
        ldNotifyDefer((LdSubCache*) tenantP->subCacheP, entityP, LdNotifyEntityUpdate, &replaceReport);
      }

      // TRoE: defer 1 entity-level "replaced" marker + N "attrReplaced"
      // events, one per attr in the new body. Attrs that existed in the
      // old body but aren't in the new body close implicitly via the
      // entity-replaced marker (read-side scopes alive windows by it).
      {
        KjNode* typeNode = kjLookup(entityP, "type");
        const char* etype = (typeNode != NULL && typeNode->type == KjString) ? typeNode->value.s : NULL;

        TroeEvent* tevP = (TroeEvent*) kaAlloc(&swRest.kalloc, sizeof(TroeEvent));
        memset(tevP, 0, sizeof(*tevP));
        tevP->op             = TroeOpEntityReplaced;
        tevP->tenantP        = tenantP;
        tevP->entityId       = entityId;
        tevP->entityType     = etype;
        tevP->modifiedAtNs   = swRest.requestStartTime;
        tevP->entitySnapshot = entityP;
        troeDeferEntityEvent(tevP);

        for (KjNode* attrP = entityP->value.firstChildP; attrP != NULL; attrP = attrP->next)
        {
          if (attrP->name == NULL)                       continue;
          if (attrP->name[0] == '@')                     continue;
          if (strcmp(attrP->name, "id")         == 0)    continue;
          if (strcmp(attrP->name, "_id")        == 0)    continue;
          if (strcmp(attrP->name, "type")       == 0)    continue;
          if (strcmp(attrP->name, "scope")      == 0)    continue;
          if (strcmp(attrP->name, "createdAt")  == 0)    continue;
          if (strcmp(attrP->name, "modifiedAt") == 0)    continue;

          TroeEvent* aevP = (TroeEvent*) kaAlloc(&swRest.kalloc, sizeof(TroeEvent));
          memset(aevP, 0, sizeof(*aevP));
          aevP->op             = TroeOpAttrReplaced;
          aevP->tenantP        = tenantP;
          aevP->entityId       = entityId;
          aevP->entityType     = etype;
          aevP->attrName       = attrP->name;
          aevP->modifiedAtNs   = swRest.requestStartTime;
          aevP->attrSnapshot   = attrP;
          aevP->entitySnapshot = entityP;
          troeDeferAttrEvent(aevP);
        }
      }
    }
    else if (r != DB_NOT_FOUND)
    {
      ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
              "database error replacing entity '%s'", entityId);
      return true;
    }
    // DB_NOT_FOUND (race with concurrent delete) tolerated when forwards landed.
  }

  //
  // Response decision matrix:
  //   - nothing succeeded AND no errors → 404 Not Found
  //   - nothing succeeded AND errors[] non-empty → 409 Conflict + body
  //   - something succeeded AND errors[] empty → 204 No Content
  //   - something succeeded AND errors[] non-empty → 207 Multi-Status + body
  //
  int errorsCount = 0;
  for (KjNode* p = errorsArrayP->value.firstChildP; p != NULL; p = p->next) errorsCount++;

  if (!anySucceeded && errorsCount == 0)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity '%s' not found", entityId);
    return true;
  }

  if (errorsCount == 0)
  {
    swRest.out.httpStatusCode = 204;
    return true;
  }

  KjNode* successArrayP = kjArray(swRest.kjsonP, "success");
  if (anySucceeded)
    kjChildAdd(successArrayP, kjString(swRest.kjsonP, NULL, entityId));

  KjNode* respBodyP = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(respBodyP, successArrayP);
  kjChildAdd(respBodyP, errorsArrayP);

  swRest.out.responseTree   = respBodyP;

  // § 6.3.5 / § 7.3.x — single authoritative source → its single-source code
  // (504/502/409), distributed over several sources → 207. See patchEntity.
  if (anySucceeded)
    swRest.out.httpStatusCode = 207;
  else if (singleAuthoritative && errorsCount == 1)
    swRest.out.httpStatusCode = (forwardFailCount == 1) ? (forwardTimedOut ? 504 : 502) : 409;
  else
    swRest.out.httpStatusCode = 207;

  return true;
}

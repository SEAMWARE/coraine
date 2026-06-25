//
// FILE            patchEntity.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strlen, strcpy, strcmp
#include <stdlib.h>                                  // free
#include <stdio.h>                                   // snprintf

#include "swRest/SwRestState.h"                      // swRest
#include "swRest/SwRestVerb.h"                       // SwVerbPatch

#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjBuilder.h"                         // kjObject, kjArray, kjString, kjChildAdd
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjRender.h"                          // kjFastRender
#include "kjson/kjRenderSize.h"                      // kjFastRenderSize

#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "swJsonld/swldInit.h"                       // swldCoreContext, SWLD_CORE_CONTEXT_URL
#include "swJsonld/swldCompactTree.h"                // swldCompactTreeWith

#include "swNgsild/swNgsild.h"                       // ldError, ldCheckEntity, LdOp*, LD_ERROR_*, swNgsild
#include "swNgsild/ldCheckEntity.h"                  // ldCheckEntity
#include "swNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "swNgsild/ldEntityMerge.h"                  // LdMergeReport
#include "swNgsild/LdProblem.h"                      // LD_ERROR_CONFLICT
#include "swNgsild/LdSubCache.h"                     // LdSubCache
#include "swNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityUpdate
#include "swNgsild/ldNotifyDefer.h"                  // ldNotifyDefer

#include "troe/troeFromMerge.h"                      // troeDeferAttrEventsFromMerge

#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode, LdRegInfo
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "swNgsild/ldDistOp.h"                       // ldDistOpLoopDetected, ldDistOpSend, ldDistOpBatchErrorAdd
#include "swNgsild/ldEntityFragment.h"               // ldEntityFragmentForInfo

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "ktrace/kTrace.h"                           // KT_T
#include "swBrokerTraceLevels.h"                     // KtDistOpRequest

#include "serviceRoutines/patchEntity.h"             // Own interface



// -----------------------------------------------------------------------------
//
// hasNonKeywordAttr - true if fragment has any top-level non-keyword attribute
//
static bool hasNonKeywordAttr(KjNode* entityP)
{
  if (entityP == NULL || entityP->type != KjObject)
    return false;

  for (KjNode* curP = entityP->value.firstChildP; curP != NULL; curP = curP->next)
  {
    if (curP->name == NULL)                       continue;
    if (curP->name[0] == '@')                     continue;
    if (strcmp(curP->name, "id")   == 0)          continue;
    if (strcmp(curP->name, "type") == 0)          continue;
    return true;
  }
  return false;
}




// -----------------------------------------------------------------------------
//
// mergeUrl - compose the PATCH forward URL for one CSR
//
// <endpoint>/ngsi-ld/v1/entities/<entityId>
//
static char* mergeUrl(const char* endpoint, const char* entityId)
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
// forwardMergeEntity - PATCH entity fragment to a CSR endpoint
//
static int forwardMergeEntity(LdRegCacheItem* csr,
                              KjNode*         fragP,
                              const char*     entityId,
                              const char*     ownAlias,
                              const char**    errorDetailPP)
{
  char* body = renderFragmentWithContext(fragP);
  return ldDistOpSend(csr, SwVerbPatch,
                      mergeUrl(csr->endpoint, entityId),
                      body, strlen(body), ownAlias, errorDetailPP);
}



// -----------------------------------------------------------------------------
//
// patchEntity -
//
bool patchEntity(void)
{
  const char* entityId = swRest.in.wildcard[0];
  KjNode*     fragment = swRest.in.requestTree;

  //
  // Validate the fragment. LdOpMergeEntity allows partial payloads (no
  // mandatory id/type) and permits "urn:ngsi-ld:null" at the top level as a
  // delete-marker.
  //
  if (ldCheckEntity(fragment, LdOpMergeEntity, NULL, &swRest.kalloc) == false)
    return true;

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;
  //
  // § 9.3.3 guard — a ?local=true write must not produce local data that an
  // exclusive or redirect registration claims.
  //
  if (swNgsild.local == true && tenantP->regCacheP != NULL)
  {
    const char* cRegId = ldRegCacheLocalWriteConflictTree((LdRegCache*) tenantP->regCacheP,
                                                          entityId, fragment, &swRest.kalloc);
    if (cRegId != NULL)
    {
      ldError(409, LD_ERROR_ALREADY_EXISTS, "Conflict",
              "local update overlaps with registration '%s' (§ 9.3.3 — no local data for an exclusive/redirect scope)",
              cRegId);
      return true;
    }
  }


  //
  // Dispatch — § 5.6.19.4 Merge Entity. Processed ONLY when not
  // ?local=true. Passes: exclusive → redirect → inclusive.
  // Auxiliary is retrieve-only (§ 4.3.6.2) — never enters write dispatch.
  //
  // Response body is a BatchOperationResult (§ 5.2.17) when any forward
  // fails or partially succeeds:
  //   { "success": [entityId], "errors": [BatchEntityError] }
  //
  KjNode* errorsArrayP = kjArray(swRest.kjsonP, "errors");
  bool    anySucceeded = false;

  bool inputHadAttrs = hasNonKeywordAttr(fragment);

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

  bool dispatch = (swNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  // Loops handled in the dispatch block: the builder marks loop-blocked CSRs
  // and the chop loop turns excl/redirect ones into 508 (§ 6.3.18); the local
  // merge still runs for any unclaimed attrs.

  if (dispatch)
  {
    //
    // Type vector for matcher — fragment may or may not carry type. A
    // PATCH body without type passes NULL so CSRs that filter by type
    // are not rejected just because the client omitted it (§ 5.6.19.4
    // says the fragment is a partial entity). In that case we rely on
    // the entity-id filter in the CSR to decide match/no-match.
    //
    KjNode* typeP = kjLookup(fragment, "type");
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

    //
    // All three modes share the same per-CSR loop body for mergeEntity.
    //   - exclusive + redirect: DETACH matching attrs from fragment so
    //     they don't get applied locally. op-not-supported → Conflict.
    //   - inclusive: CLONE matching attrs, keep them on fragment for the
    //     local merge. op-not-supported → silently skip.
    //
    LdDistOpGroup groups[] = {
      { exclV,  exclN,  "exclusive", true  },
      { redirV, redirN, "redirect",  true  },
      { inclV,  inclN,  "inclusive", false },
    };

    LdDistOpEntry* items;
    int n = ldDistOpEntriesBuild(groups, 3, ownAlias,
                                  swRest.serviceP->ldOp, "mergeEntity",
                                  entityId, /*perRi=*/true, entityId, NULL,
                                  errorsArrayP, &items);

    // Compact: drop entries whose riP yields no matching fragment.
    //
    // groups[] is iterated in order exclusive → redirect → inclusive.
    //   * Exclusive: each CSR owns its claimed attrs uniquely, so
    //     detach as we go — the chopped attrs never need to be
    //     visible to a later exclusive iteration (no two exclusives
    //     should claim the same attribute).
    //   * Redirect: multiple redirect CSRs covering the same entity
    //     are meant to ALL receive the merge — clone for each
    //     forward and do a single detach sweep after the last
    //     redirect (D008_01_red regressed when redirect chopped
    //     in-loop and the second CSR saw an empty fragment).
    //   * Inclusive: clone — the local merge below still applies
    //     these attrs.
    int kept = 0;
    for (int i = 0; i < n; i++)
    {
      bool isExclusive = (items[i].modeIdx == 0);

      KjNode* fragP = ldEntityFragmentForInfo(fragment, items[i].riP, swRest.kjsonP,
                                              /*detach=*/isExclusive);
      if (fragP == NULL) continue;

      // This riP claims at least one request attribute. If forwarding to it
      // would loop, the claimed attrs are already chopped from the fragment
      // (exclusive in-loop, redirect post-loop) so they won't be applied
      // locally — for excl/redirect that makes them undeliverable → 508
      // (§ 6.3.18); inclusive keeps its clone, so the local merge still serves
      // it and we just drop the forward.
      if (items[i].wouldLoop)
      {
        // Excl/redirect attrs are chopped (won't merge locally); flag so a
        // fully-loop-blocked merge flattens its terminal 404 to 508 (§ 6.3.18).
        if (items[i].errorMode)
          swNgsild.loopBlocked508 = true;
        continue;
      }

      // fragP is private (detached or cloned) — compact in place with the
      // per-CSR forward context before rendering the wire body.
      swldCompactTreeWith(fragP, ldDistOpForwardContext(items[i].csr));

      char* body = renderFragmentWithContext(fragP);
      items[kept] = items[i];
      items[kept].url     = mergeUrl(items[i].csr->endpoint, entityId);
      items[kept].body    = body;
      items[kept].bodyLen = strlen(body);
      KT_T(KtDistOpRequest, "forward: PATCH %s", items[kept].url);
      kept++;
    }

    // Post-loop redirect-detach: strip from the original fragment
    // every attribute the redirect legs picked up, now that all of
    // them have their clones.
    for (int i = 0; i < n; i++)
    {
      if (items[i].modeIdx != 1) continue;  // redirect only
      KjNode* drop = ldEntityFragmentForInfo(fragment, items[i].riP, swRest.kjsonP,
                                              /*detach=*/true);
      (void) drop;  // freed with the arena
    }

    ldDistOpEntriesPerform(items, kept, SwVerbPatch, ownAlias);

    for (int i = 0; i < kept; i++)
    {
      int sc = items[i].statusCode;
      if (sc >= 200 && sc < 300)
        anySucceeded = true;
      else if (sc != 404)
        ldDistOpBatchErrorAdd(errorsArrayP, entityId, (sc >= 400) ? sc : 502,
                              LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                              ldDistOpForwardFailureReason(sc, items[i].errorDetail),
                              items[i].csr->regId);
    }

    ldRegCacheMatchRelease(exclV,  exclN);
    ldRegCacheMatchRelease(redirV, redirN);
    ldRegCacheMatchRelease(inclV,  inclN);
  }

  //
  // Local merge — always attempted unless dispatch consumed every non-
  // keyword attribute. Then there's nothing to apply locally (the entity
  // may still exist locally, but the fragment has no attrs left to merge).
  //
  // DB_NOT_FOUND is tolerated when forwards have already succeeded — the
  // entity lives on a CSR, not locally, and nothing failed. When no
  // forward has landed either, DB_NOT_FOUND surfaces as the overall 404.
  //
  bool localOp = inputHadAttrs ? hasNonKeywordAttr(fragment) : true;
  int  localR  = DB_NOT_FOUND;

  if (localOp)
  {
    ldApiEntityToDbModel(fragment, &swRest.kalloc, 0);

    LdMergeReport report = { NULL };
    // Merge Entity (§ 10.2.9) — true RFC 7396 surgical merge (deepMerge=true).
    localR = db.entityMerge(tenantP, entityId, fragment, swRest.requestStartTime, &report, true);

    //
    // ldEntityMerge may have called ldError mid-merge (simplified
    // LanguageProperty without ?lang=, unsupported attribute type for a
    // simplified scalar, ...). That takes precedence over the driver's
    // return code.
    //
    if (swRest.out.problemType != NULL)
      return true;

    if (localR == DB_OK)
    {
      anySucceeded = true;

      KjNode* mergedEntity = NULL;
      if (tenantP->subCacheP != NULL)
        db.entityRetrieve(tenantP, entityId, &mergedEntity);

      if (tenantP->subCacheP != NULL && mergedEntity != NULL)
        ldNotifyDefer((LdSubCache*) tenantP->subCacheP, mergedEntity, LdNotifyEntityUpdate, &report);

      // TRoE: defer one attr event per top-level attr in the merge report.
      if (mergedEntity == NULL)
        db.entityRetrieve(tenantP, entityId, &mergedEntity);
      {
        const char* etype = NULL;
        if (mergedEntity != NULL)
        {
          KjNode* tn = kjLookup(mergedEntity, "type");
          if (tn != NULL && tn->type == KjString) etype = tn->value.s;
        }
        troeDeferAttrEventsFromMerge(tenantP, entityId, etype, mergedEntity, &report,
                                     swRest.requestStartTime);
      }
    }
    else if (localR != DB_NOT_FOUND)
    {
      ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
              "database error merging entity '%s'", entityId);
      return true;
    }
    // localR == DB_NOT_FOUND tolerated when a CSR carried the merge.
  }

  //
  // Response decision:
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
  swRest.out.httpStatusCode = anySucceeded ? 207 : 409;

  return true;
}

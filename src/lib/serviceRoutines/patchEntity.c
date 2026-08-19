//
// FILE            patchEntity.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strlen, strcpy, strcmp
#include <stdlib.h>                                  // free
#include <stdio.h>                                   // snprintf

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/CorRestVerb.h"                       // CorVerbPatch

#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjBuilder.h"                         // kjObject, kjArray, kjString, kjChildAdd
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjRender.h"                          // kjFastRender
#include "kjson/kjRenderSize.h"                      // kjFastRenderSize

#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "corJsonld/corLdInit.h"                       // corLdCoreContext, CORLD_CORE_CONTEXT_URL
#include "corJsonld/corLdCompactTree.h"                // corLdCompactTreeWith

#include "corNgsild/corNgsild.h"                       // ldError, ldCheckEntity, LdOp*, LD_ERROR_*, corNgsild
#include "corNgsild/ldCheckEntity.h"                  // ldCheckEntity
#include "corNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "corNgsild/ldEntityMerge.h"                  // LdMergeReport
#include "corNgsild/LdProblem.h"                      // LD_ERROR_CONFLICT
#include "corNgsild/LdSubCache.h"                     // LdSubCache
#include "corNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityUpdate
#include "corNgsild/ldNotifyDefer.h"                  // ldNotifyDefer

#include "troe/troeFromMerge.h"                      // troeDeferAttrEventsFromMerge

#include "corNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode, LdRegInfo
#include "corNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "corNgsild/ldDistOp.h"                       // ldDistOpLoopDetected, ldDistOpSend, ldDistOpBatchErrorAdd
#include "corNgsild/ldEntityFragment.h"               // ldEntityFragmentForInfo

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "ktrace/kTrace.h"                           // KT_T
#include "coraineTraceLevels.h"                     // KtDistOpRequest

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

  //
  // The three URL parameters of § 10.2.9.3 that change how the BODY is read —
  // format, lang, observedAt — must travel with it. The forwarded fragment is
  // the caller's own, and under ?format=simplified its bare values are still
  // bare: only the flag tells the receiving broker to resolve them against the
  // Attribute types IT holds (§ 10.2.9.4). Dropping the flag would hand a remote
  // a body meaning one thing and a request saying it means another — the value
  // would be read as a concise Property and refused as an Attribute type change.
  //
  // Attributes forwarded under an exclusive or redirect registration are exactly
  // the ones the local broker does NOT hold, so there is no local type to
  // pre-resolve them against; the flag is the only thing that can carry the
  // intent across.
  //
  const char* fmt  = (corNgsild.format == LdFormatSimplified) ? "format=simplified" : NULL;
  const char* lang = (corNgsild.lang       != NULL && corNgsild.lang[0]       != 0) ? corNgsild.lang       : NULL;
  const char* obs  = (corNgsild.observedAt != NULL && corNgsild.observedAt[0] != 0) ? corNgsild.observedAt : NULL;

  int qLen = 0;
  if (fmt  != NULL)  qLen += 1 + strlen(fmt);
  if (lang != NULL)  qLen += 1 + 5 + strlen(lang);          // "&lang=" + value
  if (obs  != NULL)  qLen += 1 + 11 + strlen(obs);          // "&observedAt=" + value

  char* url = (char*) kaAlloc(&corRest.kalloc, baseLen + pathLen + idLen + qLen + 1);
  char* p   = url;

  memcpy(p, endpoint, baseLen);  p += baseLen;
  memcpy(p, path,     pathLen);  p += pathLen;
  memcpy(p, entityId, idLen);    p += idLen;

  char sep = '?';
  if (fmt != NULL)
  {
    *p++ = sep;  sep = '&';
    int len = strlen(fmt);  memcpy(p, fmt, len);  p += len;
  }
  if (lang != NULL)
  {
    *p++ = sep;  sep = '&';
    memcpy(p, "lang=", 5);  p += 5;
    int len = strlen(lang);  memcpy(p, lang, len);  p += len;
  }
  if (obs != NULL)
  {
    *p++ = sep;
    memcpy(p, "observedAt=", 11);  p += 11;
    int len = strlen(obs);  memcpy(p, obs, len);  p += len;
  }
  *p = 0;

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
  char* buf     = (char*) kaAlloc(&corRest.kalloc, bufSize);

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
  return ldDistOpSend(csr, CorVerbPatch,
                      mergeUrl(csr->endpoint, entityId),
                      body, strlen(body), ownAlias, errorDetailPP);
}



// -----------------------------------------------------------------------------
//
// patchEntity -
//
bool patchEntity(void)
{
  const char* entityId = corRest.in.wildcard[0];
  KjNode*     fragment = corRest.in.requestTree;

  //
  // Validate the fragment. LdOpMergeEntity allows partial payloads (no
  // mandatory id/type) and permits "urn:ngsi-ld:null" at the top level as a
  // delete-marker.
  //
  if (ldCheckEntity(fragment, LdOpMergeEntity, NULL, &corRest.kalloc) == false)
    return true;

  Tenant* tenantP = (Tenant*) corNgsild.tenantP;
  //
  // § 9.3.3 guard — a ?local=true write must not produce local data that an
  // exclusive or redirect registration claims.
  //
  if (corNgsild.local == true && tenantP->regCacheP != NULL)
  {
    const char* cRegId = ldRegCacheLocalWriteConflictTree((LdRegCache*) tenantP->regCacheP,
                                                          entityId, fragment, &corRest.kalloc);
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
  KjNode* errorsArrayP = kjArray(corRest.kjsonP, "errors");
  bool    anySucceeded = false;

  // § 6.3.5 single-source error contract: when the whole operation is served by
  // exactly one exclusive/redirect source and nothing is held locally, an
  // all-failed outcome carries that source's status (504 timeout / 502 other),
  // or a genuine 409 Conflict when the op is unsupported (§ 10.2.3.4). When the
  // entity is distributed over several sources, all-failed → 207.
  bool    singleAuthoritative = false;   // exactly one exclusive/redirect source, no inclusive
  int     forwardFailCount    = 0;       // forwarded entries that failed (non-2xx, non-404)
  bool    forwardTimedOut     = false;   // that failed forward was a broker per-CSR timeout

  bool inputHadAttrs = hasNonKeywordAttr(fragment);

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);

  bool dispatch = (corNgsild.local == false
                  
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

    singleAuthoritative = ((exclN + redirN) == 1) && (inclN == 0);

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
                                  corRest.serviceP->ldOp, "mergeEntity",
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

      KjNode* fragP = ldEntityFragmentForInfo(fragment, items[i].riP, corRest.kjsonP,
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
          corNgsild.loopBlocked508 = true;
        continue;
      }

      // fragP is private (detached or cloned) — compact in place with the
      // per-CSR forward context before rendering the wire body.
      corLdCompactTreeWith(fragP, ldDistOpForwardContext(items[i].csr));

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
      KjNode* drop = ldEntityFragmentForInfo(fragment, items[i].riP, corRest.kjsonP,
                                              /*detach=*/true);
      (void) drop;  // freed with the arena
    }

    ldDistOpEntriesPerform(items, kept, CorVerbPatch, ownAlias);

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
    ldApiEntityToDbModel(fragment, &corRest.kalloc, 0);

    //
    // Merge in the broker: fetch the current entity, deep-merge the fragment
    // into it (§ 10.2.9, true RFC 7396), then ask the driver to persist the
    // resulting change report. The merge engine lives here, not in the plugin.
    //
    KjNode* mergedEntity = NULL;
    localR = db.entityRetrieve(tenantP, entityId, &mergedEntity);

    if (localR == DB_OK)
    {
      LdMergeReport report = { NULL };

      //
      // ldEntityMerge may call ldError mid-merge (simplified LanguageProperty
      // without ?lang=, unsupported attribute type for a simplified scalar,
      // attribute type change attempt, ...) and return false.
      //
      if (ldEntityMerge(mergedEntity, fragment, &report, corRest.requestStartTime, corRest.kjsonP) == false)
        return true;

      int car = db.entityChangesApply(tenantP, entityId, mergedEntity, &report);
      if (car == DB_GEO_TYPE_CONFLICT)
      {
        ldGeoTypeConflict();
        return true;
      }

      if (car == DB_INVALID_GEOMETRY)
      {
        ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid GeoProperty",
                "the merged entity carries an invalid GeoProperty geometry");
        return true;
      }
      if (car != DB_OK)
      {
        ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
                "database error merging entity '%s'", entityId);
        return true;
      }

      anySucceeded = true;

      // mergedEntity is the post-merge tree — feed notifications + TRoE directly.
      if (tenantP->subCacheP != NULL)
        ldNotifyDefer((LdSubCache*) tenantP->subCacheP, mergedEntity, LdNotifyEntityUpdate, &report);

      // TRoE: defer one attr event per top-level attr in the merge report.
      {
        const char* etype = NULL;
        KjNode* tn = kjLookup(mergedEntity, "type");
        if (tn != NULL && tn->type == KjString) etype = tn->value.s;
        troeDeferAttrEventsFromMerge(tenantP, entityId, etype, mergedEntity, &report,
                                     corRest.requestStartTime);
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
    corRest.out.httpStatusCode = 204;
    return true;
  }

  KjNode* successArrayP = kjArray(corRest.kjsonP, "success");
  if (anySucceeded)
    kjChildAdd(successArrayP, kjString(corRest.kjsonP, NULL, entityId));

  KjNode* respBodyP = kjObject(corRest.kjsonP, NULL);
  kjChildAdd(respBodyP, successArrayP);
  kjChildAdd(respBodyP, errorsArrayP);

  corRest.out.responseTree   = respBodyP;

  //
  // § 6.3.5 / § 7.3.x status code for the all-failed case:
  //   - single authoritative source (one exclusive/redirect, nothing local) →
  //     its single-source code: 504 on a broker timeout, 502 on any other
  //     gateway failure, or 409 when the op is unsupported (Conflict, § 10.2.3.4)
  //   - distributed over several sources → 207 Multi-Status
  //
  if (anySucceeded)
    corRest.out.httpStatusCode = 207;
  else if (singleAuthoritative && errorsCount == 1)
    corRest.out.httpStatusCode = (forwardFailCount == 1) ? (forwardTimedOut ? 504 : 502) : 409;
  else
    corRest.out.httpStatusCode = 207;

  return true;
}

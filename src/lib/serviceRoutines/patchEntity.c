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
// entityInfoCoversId - does any EntityInfo entry in riP cover entityId?
//
static bool entityInfoCoversId(LdRegInfo* riP, const char* entityId)
{
  for (LdRegEntityInfo* eiP = riP->entityInfoV; eiP != NULL; eiP = eiP->next)
  {
    if (eiP->id == NULL && eiP->idPatternList == NULL)
      return true;
    if (eiP->id != NULL && strcmp(eiP->id, entityId) == 0)
      return true;
    for (LdRegIdPattern* patP = eiP->idPatternList; patP != NULL; patP = patP->next)
      if (regexec(&patP->regex, entityId, 0, NULL, 0) == 0)
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
  if (kjLookup(fragP, "@context") == NULL)
  {
    KjNode* ctxNode = kjString(swRest.kjsonP, "@context", SWLD_CORE_CONTEXT_URL);
    kjChildAdd(fragP, ctxNode);
  }

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

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;   // forwards suppressed; local merge still runs below.

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
    LdRegCacheItem** groups[]   = { exclV,       redirV,     inclV      };
    int              counts[]   = { exclN,       redirN,     inclN      };
    const char*      modeTag[]  = { "exclusive", "redirect", "inclusive" };
    bool             detach[]   = { true,        true,       false      };
    bool             opConf[]   = { true,        true,       false      };

    int total = 0;
    for (int g = 0; g < 3; g++)
      for (int i = 0; i < counts[g]; i++)
        for (LdRegInfo* riP = groups[g][i]->infoV; riP != NULL; riP = riP->next) total++;

    LdDistOpBatchItem*   items   = (LdDistOpBatchItem*)   kaAlloc(&swRest.kalloc, total * sizeof(LdDistOpBatchItem));
    LdDistOpBatchResult* results = (LdDistOpBatchResult*) kaAlloc(&swRest.kalloc, total * sizeof(LdDistOpBatchResult));
    int                  itemCount = 0;
    memset(results, 0, total * sizeof(LdDistOpBatchResult));

    for (int g = 0; g < 3; g++)
    {
      for (int i = 0; i < counts[g]; i++)
      {
        LdRegCacheItem* csr = groups[g][i];
        if (csr->endpoint == NULL) continue;
        if (ldDistOpCsrWouldLoop(csr, ownAlias)) continue;

        bool opSupported = ldRegOpSupported(csr, swRest.serviceP->ldOp);

        for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
        {
          if (!entityInfoCoversId(riP, entityId)) continue;

          KjNode* fragP = ldEntityFragmentForInfo(fragment, riP, swRest.kjsonP, detach[g]);
          if (fragP == NULL) continue;

          if (!opSupported)
          {
            if (!opConf[g]) continue;
            char detail[256];
            snprintf(detail, sizeof(detail),
                     "%s registration does not support mergeEntity", modeTag[g]);
            ldDistOpBatchErrorAdd(errorsArrayP, entityId,
                                  LD_ERROR_CONFLICT, "Conflict", detail, csr->regId);
            continue;
          }

          char* body = renderFragmentWithContext(fragP);
          items[itemCount].csr     = csr;
          items[itemCount].url     = mergeUrl(csr->endpoint, entityId);
          items[itemCount].body    = body;
          items[itemCount].bodyLen = strlen(body);
          itemCount++;
        }
      }
    }

    if (itemCount > 0)
    {
      ldDistOpSendMulti(items, itemCount, SwVerbPatch, ownAlias, results);

      for (int i = 0; i < itemCount; i++)
      {
        int upCode = results[i].statusCode;
        if (upCode >= 200 && upCode < 300)
          anySucceeded = true;
        else if (upCode != 404)
          ldDistOpBatchErrorAdd(errorsArrayP, entityId,
                                LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                                ldDistOpForwardFailureReason(upCode, results[i].errorDetail),
                                items[i].csr->regId);
      }
    }

    if (exclV  != NULL) free(exclV);
    if (redirV != NULL) free(redirV);
    if (inclV  != NULL) free(inclV);
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
    ldApiEntityToDbModel(fragment, &swRest.kalloc);

    LdMergeReport report = { NULL };
    localR = db.entityMerge(tenantP, entityId, fragment, swRest.requestStartTime, &report);

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

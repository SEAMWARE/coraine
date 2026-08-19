//
// FILE            postEntitiesTemporal.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// POST /ngsi-ld/v1/temporal/entities — § 5.6.11 / § 6.18.3.1.
// Create or Update Temporal Evolution of an Entity. The body is an
// EntityTemporal — id, type, and per-attribute arrays of instances.
// We delegate to the plugin which inserts directly into the TRoE
// store, bypassing the current-state DB.
//
// Distops (§ 4.3.6 / § 5.6.11.4): forward to CSRs whose operations[]
// include "upsertTemporal". Per § 4.20 Table 4.20-2, upsertTemporal is
// NOT in any default group — CSRs must opt in explicitly.
//
//   - exclusive  → chop matching attrs out of body, forward fragment,
//                  on forward failure record BatchEntityError (data lost)
//   - redirect   → same chop+forward semantic (broker shouldn't keep)
//   - inclusive  → forward fragment (clone), keep local copy too
//
// Response: 201 Created on local-or-forwarded success; 207 Multi-Status
// with BatchOperationResult on partial success; 409 Already Exists when
// local store says the entity is already there and nothing else
// succeeded.
//

#include <stddef.h>                                  // NULL
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // free
#include <string.h>                                  // strlen, strcpy, strcat, strcmp, memset

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/corRestOutHeader.h"                  // corRestOutHeaderAdd
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjObject, kjString, kjChildAdd
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjRender.h"                          // kjFastRender
#include "kjson/kjRenderSize.h"                      // kjFastRenderSize
#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "corJsonld/corLdInit.h"                       // CORLD_CORE_CONTEXT_URL

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/ldCheckUri.h"                     // ldCheckUri
#include "corNgsild/ldEntityFragment.h"               // ldEntityFragmentForInfo
#include "corNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "corNgsild/ldDistOp.h"                       // ldDistOpSend, ldDistOpLoopDetected, ldDistOpCsrWouldLoop, ldDistOpBatchErrorAdd, ldDistOpForwardFailureReason
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant

#include "troe/TroeDriver.h"                         // troe
#include "troe/troeNotAvailable.h"                   // troeNotAvailable

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postEntitiesTemporal.h"    // Own interface



// -----------------------------------------------------------------------------
//
// hasNonKeywordAttr - true if entityP has any non-keyword child.
//
static bool hasNonKeywordAttr(KjNode* entityP)
{
  if (entityP == NULL || entityP->type != KjObject)
    return false;

  for (KjNode* c = entityP->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->name == NULL)             continue;
    if (c->name[0] == '@')           continue;
    if (strcmp(c->name, "id")   == 0) continue;
    if (strcmp(c->name, "type") == 0) continue;
    return true;
  }
  return false;
}



// -----------------------------------------------------------------------------
//
// entityInfoCoversId - does any EntityInfo in riP cover entityId?
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
// renderTemporalFragment - serialize fragment with @context for remote POST.
//
static char* renderTemporalFragment(KjNode* fragP)
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
// forwardUpsertTemporal - POST /temporal/entities to a CSR.
//
static int forwardUpsertTemporal(LdRegCacheItem* csr,
                                 KjNode*         fragP,
                                 const char*     ownAlias,
                                 const char**    errorDetailPP)
{
  const char* path    = "/ngsi-ld/v1/temporal/entities";
  int         baseLen = strlen(csr->endpoint);
  int         pathLen = strlen(path);
  char*       url     = (char*) kaAlloc(&corRest.kalloc, baseLen + pathLen + 1);
  strcpy(url, csr->endpoint);
  strcpy(url + baseLen, path);

  char* body = renderTemporalFragment(fragP);
  return ldDistOpSend(csr, CorVerbPost, url, body, strlen(body), ownAlias, errorDetailPP);
}



bool postEntitiesTemporal(void)
{
  KjNode* bodyP = corRest.in.requestTree;

  if (bodyP == NULL || bodyP->type != KjObject)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Not a JSON Object",
            "request body must be a JSON-LD object (EntityTemporal)");
    return true;
  }

  KjNode* idP   = kjLookup(bodyP, "id");
  KjNode* typeP = kjLookup(bodyP, "type");

  if (idP == NULL || idP->type != KjString || idP->value.s[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Mandatory Field Missing",
            "EntityTemporal must include a non-empty 'id'");
    return true;
  }
  if (ldCheckUri(idP->value.s) == false)
    return true;
  //
  // § 5.2.6.4.2: an Entity's type is one type name OR an array of them. Only the
  // string form used to be accepted, so an array was reported as a missing type -
  // wrong on both counts, the type WAS supplied and an array is legal.
  //
  if (typeP == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Mandatory Field Missing",
            "EntityTemporal must include a non-empty 'type'");
    return true;
  }

  if (typeP->type == KjArray)
  {
    if (typeP->value.firstChildP == NULL)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Mandatory Field Missing",
              "EntityTemporal must include a non-empty 'type'");
      return true;
    }

    for (KjNode* tP = typeP->value.firstChildP; tP != NULL; tP = tP->next)
    {
      if ((tP->type != KjString) || (tP->value.s == NULL) || (tP->value.s[0] == 0))
      {
        ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Field Value",
                "every entry of the 'type' array must be a non-empty string");
        return true;
      }
    }
  }
  else if ((typeP->type != KjString) || (typeP->value.s[0] == 0))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Mandatory Field Missing",
            "EntityTemporal must include a non-empty 'type'");
    return true;
  }

  if (troe.entityTemporalCreate == NULL)
  {
    troeNotAvailable("temporal-entity create");
    return true;
  }

  Tenant*     tenantP    = (Tenant*) corNgsild.tenantP;
  const char* entityId   = idP->value.s;
  bool        inputHadAttrs = hasNonKeywordAttr(bodyP);

  KjNode* errorsArrayP = kjArray(corRest.kjsonP, "errors");
  bool    anySucceeded = false;

  // Distop dispatch (§ 4.3.6 / § 5.6.11.4). upsertTemporal is NOT in the
  // default operations group per § 4.20 Table 4.20-2 — CSRs must opt in
  // explicitly. Auxiliary mode is retrieve-only (§ 4.3.6.2) — never
  // enters the write dispatch.
  if (!corNgsild.local && tenantP != NULL && tenantP->regCacheP != NULL)
  {
    //
    // Registration matching considers EVERY type the Entity carries, not just
    // the first - a CSR registered for the second of two types has to match.
    //
    int typeCount = 1;
    if (typeP->type == KjArray)
    {
      typeCount = 0;
      for (KjNode* tP = typeP->value.firstChildP; tP != NULL; tP = tP->next)
        typeCount++;
    }

    char**  typeArr = (char**) kaAlloc(&corRest.kalloc, sizeof(char*) * (typeCount + 1));
    int     tIx     = 0;

    if (typeP->type == KjArray)
    {
      for (KjNode* tP = typeP->value.firstChildP; tP != NULL; tP = tP->next)
        typeArr[tIx++] = tP->value.s;
    }
    else
      typeArr[tIx++] = typeP->value.s;

    typeArr[tIx] = NULL;

    LdRegCacheItem** exclV  = NULL;
    LdRegCacheItem** redirV = NULL;
    LdRegCacheItem** inclV  = NULL;
    int exclN  = ldRegCacheMatchForRetrieve((LdRegCache*) tenantP->regCacheP,
                                            entityId, typeArr,
                                            LdRegModeExclusive, &exclV);
    int redirN = ldRegCacheMatchForRetrieve((LdRegCache*) tenantP->regCacheP,
                                            entityId, typeArr,
                                            LdRegModeRedirect, &redirV);
    int inclN  = ldRegCacheMatchForRetrieve((LdRegCache*) tenantP->regCacheP,
                                            entityId, typeArr,
                                            LdRegModeInclusive, &inclV);

    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);
    bool        loopSeen = ldDistOpLoopDetected(ownAlias);

    // A loop no longer skips dispatch: phase 1 still chops the exclusive/
    // redirect attrs but records 508 per claimed attr instead of forwarding
    // (§ 6.3.18). Inclusive attrs are kept and created locally.
    LdRegCacheItem** groups[]  = { exclV, redirV, inclV };
    int              counts[]  = { exclN, redirN, inclN };
    const char*      modeTag[] = { "exclusive", "redirect", "inclusive" };
    bool             opConf[]  = { true,  true,   false };

    int total = 0;
    for (int g = 0; g < 3; g++)
      for (int i = 0; i < counts[g]; i++)
        for (LdRegInfo* riP = groups[g][i]->infoV; riP != NULL; riP = riP->next) total++;

    LdDistOpBatchItem*   items   = (LdDistOpBatchItem*)   kaAlloc(&corRest.kalloc, total * sizeof(LdDistOpBatchItem));
    memset(items, 0, total * sizeof(LdDistOpBatchItem));
    LdDistOpBatchResult* results = (LdDistOpBatchResult*) kaAlloc(&corRest.kalloc, total * sizeof(LdDistOpBatchResult));
    int                  itemCount = 0;
    memset(results, 0, total * sizeof(LdDistOpBatchResult));

    const char* tpath = "/ngsi-ld/v1/temporal/entities";

    for (int g = 0; g < 3; g++)
    {
      for (int i = 0; i < counts[g]; i++)
      {
        LdRegCacheItem* csr = groups[g][i];
        if (csr->endpoint == NULL)               continue;
        bool loop = loopSeen || ldDistOpCsrWouldLoop(csr, ownAlias);

        bool opSupported = ldRegOpSupported(csr, LdOpUpsertTemporal);

        for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
        {
          if (!entityInfoCoversId(riP, entityId)) continue;

          // Exclusive: detach in-loop. Redirect: clone, sweep after
          // the loop. Inclusive: clone for local-too semantics.
          KjNode* fragP = ldEntityFragmentForInfo(bodyP, riP, corRest.kjsonP, /*detach=*/(g == 0));
          if (fragP == NULL) continue;

          if (!opSupported)
          {
            if (!opConf[g]) continue;
            char detail[256];
            snprintf(detail, sizeof(detail),
                     "%s registration does not support upsertTemporal", modeTag[g]);
            ldDistOpBatchErrorAdd(errorsArrayP, entityId, 409,
                                  LD_ERROR_CONFLICT, "Conflict", detail, csr->regId);
            continue;
          }

          // Loop-blocked forward (§ 6.3.18): claimed attrs are chopped, so for
          // excl/redirect → 508; inclusive keeps its clone for the local create.
          if (loop)
          {
            if (opConf[g])
              ldDistOpBatchErrorAdd(errorsArrayP, entityId, 508, LD_ERROR_LOOP_DETECTED, "Loop Detected",
                                    "loop detected: registration resolves back to this broker, so upsertTemporal cannot be forwarded",
                                    csr->regId);
            continue;
          }

          int baseLen = strlen(csr->endpoint);
          int pathLen = strlen(tpath);
          char* url   = (char*) kaAlloc(&corRest.kalloc, baseLen + pathLen + 1);
          strcpy(url, csr->endpoint);
          strcpy(url + baseLen, tpath);
          char* body = renderTemporalFragment(fragP);

          items[itemCount].csr     = csr;
          items[itemCount].url     = url;
          items[itemCount].body    = body;
          items[itemCount].bodyLen = strlen(body);
          itemCount++;
        }
      }
    }

    // Post-loop redirect-detach: see the comment in postEntityAttrs.c.
    for (int i = 0; i < counts[1]; i++)
    {
      LdRegCacheItem* csr = groups[1][i];
      if (csr == NULL || csr->endpoint == NULL)  continue;
      // Detach redirect attrs whether forwarded or loop-blocked (508) — a
      // redirect-owned attr must not be created locally either way.
      for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
      {
        if (!entityInfoCoversId(riP, entityId)) continue;
        KjNode* drop = ldEntityFragmentForInfo(bodyP, riP, corRest.kjsonP, /*detach=*/true);
        (void) drop;
      }
    }

    if (itemCount > 0)
    {
      ldDistOpSendMulti(items, itemCount, CorVerbPost, ownAlias, results);

      for (int i = 0; i < itemCount; i++)
      {
        int upCode = results[i].statusCode;
        if (upCode < 200 || upCode >= 300)
          ldDistOpBatchErrorAdd(errorsArrayP, entityId, (upCode >= 400) ? upCode : 502,
                                LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                                ldDistOpForwardFailureReason(upCode, results[i].errorDetail),
                                items[i].csr->regId);
        else
          anySucceeded = true;
      }
    }

    ldRegCacheMatchRelease(exclV,  exclN);
    ldRegCacheMatchRelease(redirV, redirN);
    ldRegCacheMatchRelease(inclV,  inclN);
  }

  // Local TRoE create — skip when excl/redir consumed every input attr,
  // matching § 5.6.1.4 "any remaining input data" semantics. A pure
  // {id, type} body still creates the local temporal evolution.
  bool distopsConsumedAll = (inputHadAttrs && !hasNonKeywordAttr(bodyP));
  bool localCreatedOk     = false;
  bool localWasUpdate     = false;

  if (!distopsConsumedAll)
  {
    int r = troe.entityTemporalCreate(tenantP, bodyP);

    if (r == TROE_OK || r == TROE_UPDATED)
    {
      localCreatedOk = true;
      anySucceeded   = true;
      if (r == TROE_UPDATED)
        localWasUpdate = true;
    }
    else if (!anySucceeded)
    {
      ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
              "temporal-entity create failed");
      return true;
    }
    else
    {
      char detail[256];
      snprintf(detail, sizeof(detail),
               "local temporal create failed for entity '%s'", entityId);
      ldDistOpBatchErrorAdd(errorsArrayP, entityId, 500,
                            LD_ERROR_INTERNAL_ERROR, "Internal Error",
                            detail, NULL);
    }
  }

  // Response decision.
  int errorsCount = 0;
  for (KjNode* p = errorsArrayP->value.firstChildP; p != NULL; p = p->next) errorsCount++;

  if (errorsCount == 0)
  {
    // § 6.18.3.1: 201 Created when the temporal evolution did not exist
    // yet (Location header included), 204 No Content when this was an
    // update of a pre-existing entity (no Location header).
    if (localWasUpdate)
    {
      corRest.out.httpStatusCode = 204;
    }
    else
    {
      const char* prefix = "/ngsi-ld/v1/temporal/entities/";
      int   locLen = (int) strlen(prefix) + (int) strlen(entityId) + 1;
      char* locBuf = (char*) kaAlloc(&corRest.kalloc, locLen);
      strcpy(locBuf, prefix);
      strcat(locBuf, entityId);
      corRestOutHeaderAdd("Location", locBuf);

      corRest.out.httpStatusCode = 201;
    }
    return true;
  }

  // Mixed result → 207 with BatchOperationResult; total failure → 502.
  KjNode* result = kjObject(corRest.kjsonP, NULL);
  KjNode* successArr = kjArray(corRest.kjsonP, "success");
  if (anySucceeded)
    kjChildAdd(successArr, kjString(corRest.kjsonP, NULL, entityId));
  kjChildAdd(result, successArr);
  kjChildAdd(result, errorsArrayP);

  corRest.out.responseTree   = result;
  corRest.out.httpStatusCode = anySucceeded ? 207 : 502;
  (void) localCreatedOk;
  return true;
}

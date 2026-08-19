//
// FILE            postEntityBatchMerge.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// POST /ngsi-ld/v1/entityOperations/merge — Batch Entity Merge (§ 5.6.10).
//
// Semantics match Merge Attribute (§ 5.6.6) per-entity, extended to a
// list. Merge is surgical: only attributes named in the fragment are
// touched; null values remove. Unlike Update, unrelated attributes on
// the target entity are untouched.
//
// Multi-instance same id in one batch: fragments are applied in array
// order (first = oldest, last = newest). Each fragment's merge is
// processed individually by the DB plugin (retrieve + apply + write)
// so null-delete-then-reset sequences are correct.
//
// Flow:
//
//   Pass 1 — validate + group by id (array order preserved).
//   Pass 2 — per fragment: distops chop + accumulate non-empty local
//            fragments into localFragsArr.
//   Pass 3 — synchronous distops forward per CSR.
//   Pass 4 — db.entityBulkRetrieve fetches all current docs, the broker
//            ldEntityMerge's each fragment into its target (array order),
//            then db.entityBulkChangesApply persists the surgical change-sets.
//   Pass 5 — defer-notify per successful fragment (in array order).
//   Pass 6 — response: BatchOperationResult § 5.2.17. Dedup success[]
//            by id.
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp, strlen, strncpy, strcasecmp, strcpy
#include <strings.h>                                 // strcasecmp
#include <stdlib.h>                                  // free
#include <stdio.h>                                   // snprintf

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/CorRestVerb.h"                       // CorVerbPost

#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjArray, kjString, kjChildAdd, kjChildRemove
#include "kjson/kjClone.h"                           // kjClone
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjParse.h"                           // kjParse
#include "kjson/kjRender.h"                          // kjFastRender
#include "kjson/kjRenderSize.h"                      // kjFastRenderSize

#include "corJsonld/corLdInit.h"                       // CORLD_CORE_CONTEXT_URL
#include "corJsonld/corLdDownload.h"                   // corLdContextFromUrl
#include "corJsonld/corLdCompactTree.h"                // corLdCompactTreeWith

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdOp.h"                           // LdOpMergeEntity, LdOpBatchMerge
#include "corNgsild/LdNormalizeInput.h"               // ldNormalizeInput
#include "corNgsild/ldCheckEntity.h"                  // ldCheckEntity
#include "corNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "corNgsild/ldStripAtContext.h"              // ldStripAtContext
#include "corNgsild/LdProblem.h"                      // LD_ERROR_RESOURCE_NOT_FOUND, LD_ERROR_CONFLICT, LD_ERROR_INTERNAL_ERROR
#include "corNgsild/ldEntityMerge.h"                  // LdMergeReport
#include "corNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityUpdate
#include "corNgsild/ldNotifyDefer.h"                  // ldNotifyDefer

#include "troe/troeFromMerge.h"                      // troeDeferAttrEventsFromMerge
#include "corNgsild/LdSubCache.h"                     // LdSubCache

#include "corNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode
#include "corNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "corNgsild/ldDistOp.h"                       // ldDistOp*
#include "corNgsild/ldEntityFragment.h"               // ldEntityFragmentForInfo

#include "kalloc/kaStrdup.h"                          // kaStrdup
#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND, DB_ERR, DB_BAD_INPUT
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postEntityBatchMerge.h"    // Own interface



// -----------------------------------------------------------------------------
//
// addBatchError - append a BatchEntityError (§ 5.2.17) to errors[].
//
static void addBatchError(KjNode* errorsP, const char* entityId, int statusCode,
                          const char* errType, const char* title,
                          const char* detail, const char* regId)
{
  KjNode* err = kjObject(corRest.kjsonP, NULL);
  kjChildAdd(err, kjString(corRest.kjsonP, "entityId", (char*) entityId));

  KjNode* pd = kjObject(corRest.kjsonP, "error");
  kjChildAdd(pd, kjString (corRest.kjsonP, "type",   (char*) errType));
  kjChildAdd(pd, kjString (corRest.kjsonP, "title",  (char*) title));
  kjChildAdd(pd, kjInteger(corRest.kjsonP, "status", statusCode));
  kjChildAdd(pd, kjString (corRest.kjsonP, "detail", (char*) detail));
  kjChildAdd(err, pd);

  if (regId != NULL)
    kjChildAdd(err, kjString(corRest.kjsonP, "registrationId", (char*) regId));

  kjChildAdd(errorsP, err);
}



static char* renderBatchBody(LdRegCacheItem* csr, KjNode* batchArr)
{
  //
  // § 4.3.6.6: compact every fragment against the effective forward
  // context — csi.jsonldContext > incoming request @context > core
  // (ldDistOpForwardContext: the same context buildHeaders names in the
  // Link header) — and strip in-body @context (the forward goes out as
  // application/json + Link).
  //
  CorLdContext* fwdCtx = ldDistOpForwardContext(csr);

  for (KjNode* fragP = batchArr->value.firstChildP; fragP != NULL; fragP = fragP->next)
  {
    corLdCompactTreeWith(fragP, fwdCtx);

    KjNode* atCtx = kjLookup(fragP, "@context");
    if (atCtx != NULL)
      kjChildRemove(fragP, atCtx);
  }

  int   bufSize = kjFastRenderSize(batchArr) + 1;
  char* buf     = (char*) kaAlloc(&corRest.kalloc, bufSize);
  kjFastRender(batchArr, buf);
  return buf;
}



static int forwardBatchToCSR(LdRegCacheItem* csr, KjNode* batchArr,
                              const char* ownAlias, KjNode** respTreePP)
{
  *respTreePP = NULL;

  const char* path    = "/ngsi-ld/v1/entityOperations/merge";
  int         baseLen = strlen(csr->endpoint);
  int         pathLen = strlen(path);
  char*       url     = (char*) kaAlloc(&corRest.kalloc, baseLen + pathLen + 1);
  strcpy(url, csr->endpoint);
  strcpy(url + baseLen, path);

  char* body    = renderBatchBody(csr, batchArr);
  int   bodyLen = strlen(body);

  char*       respBody    = NULL;
  int         respBodyLen = 0;
  const char* upErr       = NULL;

  int status = ldDistOpSendReceive(csr, CorVerbPost, url, body, bodyLen,
                                    ownAlias, &upErr,
                                    &respBody, &respBodyLen);

  if (respBody != NULL && respBodyLen > 0)
  {
    KjNode* treeP = kjParse(corRest.kjsonP, respBody);
    if (treeP != NULL)
    {
      ldStripAtContext(treeP);
      *respTreePP = treeP;
    }
  }

  return status;
}



static void applyRemoteBatchResult(int status, KjNode* respTreeP,
                                    const char* csrRegId,
                                    KjNode* errorsP,
                                    bool* anyOkV,
                                    const char** idV, int N)
{
  bool success2xx = (status >= 200 && status < 300);

  if (success2xx && respTreeP == NULL)
  {
    for (int i = 0; i < N; i++) anyOkV[i] = true;
    return;
  }

  if (!success2xx || respTreeP == NULL)
  {
    char detail[256];
    snprintf(detail, sizeof(detail), "forward to '%s' failed (status %d)",
             csrRegId ? csrRegId : "?", status);
    for (int i = 0; i < N; i++)
      addBatchError(errorsP, idV[i], (status >= 400) ? status : 502,
                    LD_ERROR_INTERNAL_ERROR, "Bad Gateway", detail, csrRegId);
    return;
  }

  KjNode* remoteSuccess = kjLookup(respTreeP, "success");
  KjNode* remoteErrors  = kjLookup(respTreeP, "errors");

  if (remoteSuccess != NULL && remoteSuccess->type == KjArray)
  {
    for (KjNode* sP = remoteSuccess->value.firstChildP; sP != NULL; sP = sP->next)
    {
      if (sP->type != KjString) continue;
      for (int i = 0; i < N; i++)
        if (strcmp(idV[i], sP->value.s) == 0) { anyOkV[i] = true; break; }
    }
  }

  if (remoteErrors != NULL && remoteErrors->type == KjArray)
  {
    for (KjNode* eP = remoteErrors->value.firstChildP; eP != NULL; eP = eP->next)
    {
      KjNode* idP     = kjLookup(eP, "entityId");
      KjNode* errP    = kjLookup(eP, "error");
      const char* eid = (idP != NULL && idP->type == KjString) ? idP->value.s : "";

      const char* type   = LD_ERROR_INTERNAL_ERROR;
      const char* title  = "Bad Gateway";
      const char* detail = "forward error";
      int         status = 502;
      if (errP != NULL && errP->type == KjObject)
      {
        KjNode* tP = kjLookup(errP, "type");
        KjNode* hP = kjLookup(errP, "title");
        KjNode* dP = kjLookup(errP, "detail");
        KjNode* sP = kjLookup(errP, "status");
        if (tP != NULL && tP->type == KjString)  type   = tP->value.s;
        if (hP != NULL && hP->type == KjString)  title  = hP->value.s;
        if (dP != NULL && dP->type == KjString)  detail = dP->value.s;
        if (sP != NULL && sP->type == KjInt)     status = sP->value.i;
      }
      addBatchError(errorsP, eid, status, type, title, detail, csrRegId);
    }
  }
}



// -----------------------------------------------------------------------------
//
// CsrAccum -
//
typedef struct CsrAccum
{
  LdRegCacheItem* csr;
  LdRegMode       mode;
  KjNode**        fragV;
  const char**    idV;
  int             count;
  int             capacity;
} CsrAccum;



static CsrAccum* csrAccumFindOrCreate(CsrAccum** aV, int* aN, int* aCap,
                                      LdRegCacheItem* csr, LdRegMode mode)
{
  for (int i = 0; i < *aN; i++)
    if ((*aV)[i].csr == csr)
      return &(*aV)[i];

  if (*aN >= *aCap)
  {
    int newCap = (*aCap == 0) ? 4 : *aCap * 2;
    CsrAccum* newV = (CsrAccum*) kaAlloc(&corRest.kalloc, newCap * sizeof(CsrAccum));
    for (int i = 0; i < *aN; i++) newV[i] = (*aV)[i];
    *aV   = newV;
    *aCap = newCap;
  }

  (*aV)[*aN].csr      = csr;
  (*aV)[*aN].mode     = mode;
  (*aV)[*aN].fragV    = NULL;
  (*aV)[*aN].idV      = NULL;
  (*aV)[*aN].count    = 0;
  (*aV)[*aN].capacity = 0;
  return &(*aV)[(*aN)++];
}



static void csrAccumAppend(CsrAccum* a, KjNode* fragP, const char* id)
{
  if (a->count >= a->capacity)
  {
    int newCap = (a->capacity == 0) ? 4 : a->capacity * 2;
    KjNode**     newF  = (KjNode**)     kaAlloc(&corRest.kalloc, newCap * sizeof(KjNode*));
    const char** newId = (const char**) kaAlloc(&corRest.kalloc, newCap * sizeof(char*));
    for (int i = 0; i < a->count; i++)
    {
      newF[i]  = a->fragV[i];
      newId[i] = a->idV[i];
    }
    a->fragV    = newF;
    a->idV      = newId;
    a->capacity = newCap;
  }
  a->fragV[a->count]  = fragP;
  a->idV[a->count]    = id;
  a->count++;
}



static void chopForMode(Tenant*      tenantP,
                         const char*  entityId,
                         char**       typeArr,
                         char**       scopeV,
                         KjNode*      fragP,
                         LdRegMode    mode,
                         bool         detach,
                         CsrAccum**   aVp,
                         int*         aNp,
                         int*         aCapP,
                         const char*  ownAlias)
{
  if (tenantP->regCacheP == NULL)
    return;

  LdRegCacheItem** matchV = NULL;
  int matchN = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                 entityId, typeArr, scopeV,
                                                 mode, &matchV);

  for (int m = 0; m < matchN; m++)
  {
    LdRegCacheItem* csr = matchV[m];
    if (csr->endpoint == NULL)                 continue;
    if (ldDistOpCsrWouldLoop(csr, ownAlias))   continue;

    for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
    {
      KjNode* chopped = ldEntityFragmentForInfo(fragP, riP, corRest.kjsonP, detach);
      if (chopped == NULL)
        continue;

      CsrAccum* a = csrAccumFindOrCreate(aVp, aNp, aCapP, csr, mode);
      csrAccumAppend(a, chopped, entityId);
    }
  }

  ldRegCacheMatchRelease(matchV, matchN);
}



//
// purgeRedirAttrsFromFragment - strip from `fragP` every attribute
// that any redirect-matched CSR claims, AFTER the redirect chopForMode
// call has run with detach=false. The detach has to be deferred until
// all redirect CSRs covering this entity have been served, otherwise the
// second-and-onwards ones find an empty fragment (D008_01_red-class
// bug for the batch-merge path).
//
static void purgeRedirAttrsFromFragment(Tenant*     tenantP,
                                         const char* entityId,
                                         char**      typeArr,
                                         char**      scopeV,
                                         KjNode*     fragP,
                                         const char* ownAlias)
{
  if (tenantP->regCacheP == NULL)
    return;

  LdRegCacheItem** matchV = NULL;
  int matchN = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                 entityId, typeArr, scopeV,
                                                 LdRegModeRedirect, &matchV);

  for (int m = 0; m < matchN; m++)
  {
    LdRegCacheItem* csr = matchV[m];
    if (csr->endpoint == NULL)                 continue;
    if (ldDistOpCsrWouldLoop(csr, ownAlias))   continue;

    for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
      (void) ldEntityFragmentForInfo(fragP, riP, corRest.kjsonP, /*detach=*/true);
  }

  ldRegCacheMatchRelease(matchV, matchN);
}



static bool hasAnyNonKeywordAttr(KjNode* fragP)
{
  if (fragP == NULL || fragP->type != KjObject) return false;
  for (KjNode* c = fragP->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->name == NULL)                 continue;
    if (c->name[0] == '@')               continue;
    if (strcmp(c->name, "id")   == 0)    continue;
    if (strcmp(c->name, "type") == 0)    continue;
    return true;
  }
  return false;
}



// -----------------------------------------------------------------------------
//
// postEntityBatchMerge -
//
bool postEntityBatchMerge(void)
{
  KjNode* bodyP = corRest.in.requestTree;

  if (bodyP->type != KjArray)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Not a JSON Array",
            "Batch Entity Merge body must be a JSON array");
    return true;
  }

  int total = 0;
  for (KjNode* c = bodyP->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->type == KjNull)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Array Entry",
              "Batch Entity Merge: null entry at position %d", total);
      return true;
    }
    total++;
  }

  bool hasPreErrors = (corNgsild.batchPreErrors != NULL &&
                       corNgsild.batchPreErrors->value.firstChildP != NULL);
  if (total == 0 && !hasPreErrors)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Empty Array",
            "Batch Entity Merge: input array is empty");
    return true;
  }

  KjNode* successP = kjArray(corRest.kjsonP, "success");
  KjNode* errorsP  = kjArray(corRest.kjsonP, "errors");

  if (hasPreErrors)
  {
    errorsP->value.firstChildP = corNgsild.batchPreErrors->value.firstChildP;
    errorsP->lastChild         = corNgsild.batchPreErrors->lastChild;
    corNgsild.batchPreErrors    = NULL;
  }

  //
  // Pass 1 — validate + extract id per fragment. We keep the array order
  // and the fragment list flat; same-id duplicates are applied in order
  // by the DB plugin.
  //
  KjNode**     fragV   = (KjNode**)     kaAlloc(&corRest.kalloc, sizeof(KjNode*) * total);
  const char** idV     = (const char**) kaAlloc(&corRest.kalloc, sizeof(char*)  * total);
  int          fragN   = 0;

  for (KjNode* inP = bodyP->value.firstChildP; inP != NULL; inP = inP->next)
  {
    if (inP->type != KjObject)
    {
      addBatchError(errorsP, "", 400,
                    LD_ERROR_BAD_REQUEST_DATA, "Invalid Array Entry",
                    "entity must be a JSON object", NULL);
      continue;
    }

    ldNormalizeInput(inP, &corRest.kalloc, false, false);

    if (ldCheckEntity(inP, LdOpBatchMerge, NULL, &corRest.kalloc) == false)
    {
      const char* eid = "";
      KjNode* idP = kjLookup(inP, "id");
      if (idP != NULL && idP->type == KjString) eid = idP->value.s;

      char snapshot[512];
      strncpy(snapshot, corRest.out.problemDetail, sizeof(snapshot) - 1);
      snapshot[sizeof(snapshot) - 1] = 0;

      addBatchError(errorsP, eid, 400,
                    LD_ERROR_BAD_REQUEST_DATA, "Invalid Entity", snapshot, NULL);

      corRest.out.httpStatusCode   = 0;
      corRest.out.problemType      = NULL;
      corRest.out.problemTitle     = NULL;
      corRest.out.problemDetail[0] = 0;
      continue;
    }

    KjNode* idP = kjLookup(inP, "id");
    if (idP == NULL || idP->type != KjString)
    {
      addBatchError(errorsP, "", 400,
                    LD_ERROR_BAD_REQUEST_DATA, "Invalid Array Entry",
                    "entity id is missing or not a string", NULL);
      continue;
    }

    //
    // § 9.3.3 guard — a ?local=true write must not produce local data that
    // an exclusive or redirect registration claims.
    //
    if (corNgsild.local == true && ((Tenant*) corNgsild.tenantP)->regCacheP != NULL)
    {
      const char* cRegId = ldRegCacheLocalWriteConflictTree((LdRegCache*) ((Tenant*) corNgsild.tenantP)->regCacheP,
                                                            idP->value.s, inP, &corRest.kalloc);
      if (cRegId != NULL)
      {
        addBatchError(errorsP, idP->value.s, 409,
                      LD_ERROR_ALREADY_EXISTS, "Conflict",
                      "local write overlaps with registration (§ 9.3.3 — no local data for an exclusive/redirect scope)",
                      cRegId);
        continue;
      }
    }

    fragV[fragN] = inP;
    idV[fragN]   = idP->value.s;
    fragN++;
  }

  if (fragN == 0)
  {
    int singleStatus = ldBatchErrorsSingleStatus(errorsP);
    if (singleStatus > 0)
    {
      corRest.out.responseTree   = ldBatchErrorAsProblemDetails(errorsP);
      corRest.out.httpStatusCode = singleStatus;
    }
    else
    {
      KjNode* respBodyP = kjObject(corRest.kjsonP, NULL);
      kjChildAdd(respBodyP, successP);
      kjChildAdd(respBodyP, errorsP);
      corRest.out.responseTree   = respBodyP;
      corRest.out.httpStatusCode = 207;
      corNgsild.rawResponse      = true;
    }
    return true;
  }

  //
  // Pass 2 — distops chop per fragment in array order. Build the flat
  // localFragsArr of fragments whose non-keyword attrs survived the
  // chop and therefore need a local merge.
  //
  Tenant* tenantP = (Tenant*) corNgsild.tenantP;

  LdSubCache* subCacheP = (LdSubCache*) tenantP->subCacheP;

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);

  bool dispatch = (corNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;

  CsrAccum* csrAccums    = NULL;
  int       csrAccumsN   = 0;
  int       csrAccumsCap = 0;

  KjNode*      localFragsArr = kjArray(corRest.kjsonP, NULL);
  const char** localIdV      = (const char**) kaAlloc(&corRest.kalloc, sizeof(char*) * fragN);
  int          localN        = 0;

  // anySuccessV: per unique id flag — true if any local or distop succeeded for that id.
  // Unique ids live in uniqueIdV; uniqueIdN is the count.
  const char** uniqueIdV  = (const char**) kaAlloc(&corRest.kalloc, sizeof(char*) * fragN);
  bool*        anySuccessV = (bool*)        kaAlloc(&corRest.kalloc, sizeof(bool)  * fragN);
  int          uniqueIdN  = 0;

  for (int i = 0; i < fragN; i++)
  {
    KjNode*     fragP = fragV[i];
    const char* id    = idV[i];

    // Record unique id if new
    bool alreadySeen = false;
    for (int k = 0; k < uniqueIdN; k++)
      if (strcmp(uniqueIdV[k], id) == 0) { alreadySeen = true; break; }
    if (!alreadySeen)
    {
      uniqueIdV[uniqueIdN]   = id;
      anySuccessV[uniqueIdN] = false;
      uniqueIdN++;
    }

    if (dispatch)
    {
      KjNode* typeP = kjLookup(fragP, "type");
      char*   typeArr[2] = { NULL, NULL };
      if (typeP != NULL && typeP->type == KjString)
        typeArr[0] = typeP->value.s;

      KjNode* scopeP       = kjLookup(fragP, "scope");
      char*   scopeBuf[2]  = { NULL, NULL };
      char**  scopeV       = NULL;
      if (scopeP != NULL && scopeP->type == KjString)
      {
        scopeBuf[0] = scopeP->value.s;
        scopeV      = scopeBuf;
      }

      // Exclusive: detach as we accumulate — each excl CSR owns its
      // attrs uniquely. Redirect: clone (multiple redirect CSRs may
      // cover the same entity, all must receive a copy), then
      // purgeRedirAttrsFromFragment chops the redirect-claimed
      // attrs once. Inclusive: clone — local merge keeps them.
      chopForMode(tenantP, id, typeArr, scopeV, fragP,
                  LdRegModeExclusive, /*detach=*/true,
                  &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
      chopForMode(tenantP, id, typeArr, scopeV, fragP,
                  LdRegModeRedirect,  /*detach=*/false,
                  &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
      purgeRedirAttrsFromFragment(tenantP, id, typeArr, scopeV, fragP, ownAlias);
      chopForMode(tenantP, id, typeArr, scopeV, fragP,
                  LdRegModeInclusive, /*detach=*/false,
                  &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
    }

    if (!hasAnyNonKeywordAttr(fragP))
      continue;

    ldApiEntityToDbModel(fragP, &corRest.kalloc, 0);

    kjChildAdd(localFragsArr, fragP);
    localIdV[localN++] = id;
  }

  //
  // Pass 3 — concurrent distops forward per CSR via ldDistOpSendMulti.
  //
  {
    LdDistOpBatchItem*   bItems   = (LdDistOpBatchItem*)   kaAlloc(&corRest.kalloc, csrAccumsN * sizeof(LdDistOpBatchItem));
    memset(bItems, 0, csrAccumsN * sizeof(LdDistOpBatchItem));
    LdDistOpBatchResult* bResults = (LdDistOpBatchResult*) kaAlloc(&corRest.kalloc, csrAccumsN * sizeof(LdDistOpBatchResult));
    int                  bIdx[csrAccumsN];
    int                  bCount   = 0;
    memset(bResults, 0, csrAccumsN * sizeof(LdDistOpBatchResult));

    const char* batchPath = "/ngsi-ld/v1/entityOperations/merge";
    int         batchPathLen = strlen(batchPath);

    for (int ai = 0; ai < csrAccumsN; ai++)
    {
      CsrAccum*       a   = &csrAccums[ai];
      LdRegCacheItem* csr = a->csr;

      if (a->count == 0) continue;

      if (!ldRegOpSupported(csr, LdOpBatchMerge))
      {
        if (a->mode == LdRegModeExclusive || a->mode == LdRegModeRedirect)
        {
          const char* detail = (a->mode == LdRegModeExclusive)
                               ? "exclusive registration does not support mergeBatch"
                               : "redirect registration does not support mergeBatch";
          for (int i = 0; i < a->count; i++)
            addBatchError(errorsP, a->idV[i], 409,
                          LD_ERROR_CONFLICT, "Conflict", detail, csr->regId);
        }
        continue;
      }

      KjNode* batchArr = kjArray(corRest.kjsonP, NULL);
      for (int i = 0; i < a->count; i++)
        kjChildAdd(batchArr, a->fragV[i]);

      int   baseLen = strlen(csr->endpoint);
      char* url     = (char*) kaAlloc(&corRest.kalloc, baseLen + batchPathLen + 1);
      strcpy(url, csr->endpoint);
      strcpy(url + baseLen, batchPath);

      char* body = renderBatchBody(csr, batchArr);

      bItems[bCount].csr     = csr;
      bItems[bCount].url     = url;
      bItems[bCount].body    = body;
      bItems[bCount].bodyLen = strlen(body);
      bIdx[bCount]           = ai;
      bCount++;
    }

    if (bCount > 0)
    {
      ldDistOpSendMulti(bItems, bCount, CorVerbPost, ownAlias, bResults);

      for (int bi = 0; bi < bCount; bi++)
      {
        CsrAccum* a = &csrAccums[bIdx[bi]];

        KjNode* respTreeP = NULL;
        if (bResults[bi].responseBody != NULL && bResults[bi].responseBodyLen > 0)
        {
          KjNode* treeP = bResults[bi].responseTree;
          if (treeP != NULL)
          {
            ldStripAtContext(treeP);
            respTreeP = treeP;
          }
        }

        bool* groupOk = (bool*) kaAlloc(&corRest.kalloc, sizeof(bool) * a->count);
        for (int k = 0; k < a->count; k++) groupOk[k] = false;

        applyRemoteBatchResult(bResults[bi].statusCode, respTreeP, bItems[bi].csr->regId,
                               errorsP, groupOk, a->idV, a->count);

        for (int k = 0; k < a->count; k++)
        {
          if (!groupOk[k]) continue;
          for (int ui = 0; ui < uniqueIdN; ui++)
          {
            if (strcmp(uniqueIdV[ui], a->idV[k]) == 0)
            {
              anySuccessV[ui] = true;
              break;
            }
          }
        }
      }
    }
  }

  //
  // Pass 4 — bulk DB merge. The plugin loops per fragment in array order,
  // applying each surgical merge. Returns per-fragment result + report +
  // post-merge snapshot.
  //
  if (localN > 0)
  {
    if (db.entityBulkRetrieve == NULL || db.entityBulkChangesApply == NULL)
    {
      ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
              "Batch Entity Merge not supported by this DB plugin");
      return true;
    }

    int*            resultsV    = (int*)            kaAlloc(&corRest.kalloc, sizeof(int)             * localN);
    LdMergeReport*  reportsV    = (LdMergeReport*)  kaAlloc(&corRest.kalloc, sizeof(LdMergeReport)   * localN);
    KjNode**        snapshotsV  = (KjNode**)        kaAlloc(&corRest.kalloc, sizeof(KjNode*)         * localN);
    KjNode**        targetsV    = (KjNode**)        kaAlloc(&corRest.kalloc, sizeof(KjNode*)         * localN);

    for (int k = 0; k < localN; k++)
    {
      reportsV[k].changes = NULL;
      snapshotsV[k]       = NULL;
      targetsV[k]         = NULL;
      resultsV[k]         = DB_NOT_FOUND;
    }

    //
    // Phase 1 — fetch every current doc (one $in for mongoc). Then merge each
    // fragment into its fetched target HERE in the broker, in array order so
    // same-id fragments accumulate. Phase 2/3 — persist the change-sets.
    // The merge engine lives in the broker; the driver only fetches and writes.
    //
    db.entityBulkRetrieve(tenantP, localFragsArr, targetsV);

    int fi = 0;
    for (KjNode* fragP = localFragsArr->value.firstChildP; fragP != NULL; fragP = fragP->next, fi++)
    {
      if (targetsV[fi] == NULL)
      {
        resultsV[fi] = DB_NOT_FOUND;
        continue;
      }

      // Batch Merge = true RFC 7396 deep-merge (§ 5.6.10 → § 10.2.9). Like the
      // single Merge Entity it may NOT change an attribute's type (§ 10.3.5
      // delegates to the Merge Entity behaviour). ldEntityMerge rejects such a
      // fragment — and any other mid-merge problem — by returning false and
      // setting the error. Capture it per-fragment NOW (the next fragment
      // overwrites the shared problemDetail buffer), then skip persisting this
      // one: no extra DB access, the single bulk write already skips non-DB_OK
      // slots.
      if (ldEntityMerge(targetsV[fi], fragP, &reportsV[fi], corRest.requestStartTime, corRest.kjsonP) == false)
      {
        KjNode*     fidP = kjLookup(fragP, "id");
        const char* fid  = (fidP != NULL && fidP->type == KjString) ? fidP->value.s : "";
        int         st   = (corRest.out.httpStatusCode >= 400) ? corRest.out.httpStatusCode : 400;
        addBatchError(errorsP, fid, st, corRest.out.problemType, corRest.out.problemTitle,
                      kaStrdup(&corRest.kalloc, corRest.out.problemDetail), NULL);
        resultsV[fi] = DB_BAD_INPUT;   // != DB_OK → bulk write skips; switch skips (already reported)
        continue;
      }
      snapshotsV[fi] = targetsV[fi];
      resultsV[fi]   = DB_OK;
    }

    db.entityBulkChangesApply(tenantP, localFragsArr, targetsV, reportsV, resultsV);

    //
    // Pass 5 — per-fragment notify + per-unique-id success tracking.
    //
    for (int k = 0; k < localN; k++)
    {
      const char* eid = localIdV[k];

      switch (resultsV[k])
      {
        case DB_OK:
        {
          // Mark success on the unique-id slot
          for (int ui = 0; ui < uniqueIdN; ui++)
            if (strcmp(uniqueIdV[ui], eid) == 0) { anySuccessV[ui] = true; break; }

          if (subCacheP != NULL && snapshotsV[k] != NULL)
            ldNotifyDefer(subCacheP, snapshotsV[k],
                          LdNotifyEntityUpdate, &reportsV[k]);

          // TRoE: per-fragment attr events for the entities the bulk
          // actually persisted.
          if (snapshotsV[k] != NULL)
          {
            KjNode* tn = kjLookup(snapshotsV[k], "type");
            const char* etype = (tn != NULL && tn->type == KjString) ? tn->value.s : NULL;
            troeDeferAttrEventsFromMerge(tenantP, eid, etype, snapshotsV[k], &reportsV[k],
                                         corRest.requestStartTime);
          }
          break;
        }
        case DB_NOT_FOUND:
          addBatchError(errorsP, eid, 404,
                        LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                        "entity does not exist", NULL);
          break;
        case DB_BAD_INPUT:
          // The fragment was rejected by ldEntityMerge (e.g. an attribute
          // type change) and already reported with its precise per-fragment
          // detail in the merge loop above — nothing more to do here.
          break;
        case DB_GEO_TYPE_CONFLICT:
          addBatchError(errorsP, eid, 409,
                        LD_ERROR_CONFLICT, "Attribute Type Conflict",
                        "an Attribute name is already in use with a conflicting Attribute type in this tenant "
                        "(a GeoProperty and another type cannot share one Attribute name here)", NULL);
          break;
        default:
          addBatchError(errorsP, eid, 500,
                        LD_ERROR_INTERNAL_ERROR, "Internal Error",
                        "database error during batch merge", NULL);
          break;
      }
    }
  }

  //
  // Pass 6 — response assembly. Dedup success[] by unique id.
  //
  int successCount = 0;
  for (int ui = 0; ui < uniqueIdN; ui++)
  {
    if (!anySuccessV[ui]) continue;
    kjChildAdd(successP, kjString(corRest.kjsonP, NULL, (char*) uniqueIdV[ui]));
    successCount++;
  }

  int errorCount = 0;
  for (KjNode* p = errorsP->value.firstChildP; p != NULL; p = p->next) errorCount++;

  // § 5.6.17 — batch ops return 204 when there are no errors. The
  // successCount==0 && errorCount==0 case (e.g. an "empty" merge fragment
  // that touches nothing) is also a success: nothing failed.
  if (errorCount == 0)
  {
    corRest.out.httpStatusCode = 204;
    return true;
  }

  // Partial-success → 207 with the BatchOperationResult body. All-failed-
  // with-one-cause → that cause's plain status + a plain Problem Details
  // body (cloned from the first error entry); easier on clients than 207
  // wrapping a uniform error in a multi-status envelope.
  int singleStatus = (successCount == 0) ? ldBatchErrorsSingleStatus(errorsP) : -1;
  if (singleStatus > 0)
  {
    corRest.out.responseTree   = ldBatchErrorAsProblemDetails(errorsP);
    corRest.out.httpStatusCode = singleStatus;
  }
  else
  {
    KjNode* respBodyP = kjObject(corRest.kjsonP, NULL);
    kjChildAdd(respBodyP, successP);
    kjChildAdd(respBodyP, errorsP);
    corRest.out.responseTree   = respBodyP;
    corRest.out.httpStatusCode = 207;
  }
  corNgsild.rawResponse      = true;
  return true;
}

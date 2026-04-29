//
// FILE            postEntityBatchMerge.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
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
//   Pass 4 — db.entityBulkMerge(localFragsArr) — plugin loops per
//            fragment doing surgical merge; returns report + snapshot
//            per fragment.
//   Pass 5 — defer-notify per successful fragment (in array order).
//   Pass 6 — response: BatchOperationResult § 5.2.17. Dedup success[]
//            by id.
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp, strlen, strncpy, strcasecmp, strcpy
#include <strings.h>                                 // strcasecmp
#include <stdlib.h>                                  // free
#include <stdio.h>                                   // snprintf

#include "swRest/SwRestState.h"                      // swRest
#include "swRest/SwRestVerb.h"                       // SwVerbPost

#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjArray, kjString, kjChildAdd, kjChildRemove
#include "kjson/kjClone.h"                           // kjClone
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjParse.h"                           // kjParse
#include "kjson/kjRender.h"                          // kjFastRender
#include "kjson/kjRenderSize.h"                      // kjFastRenderSize

#include "swJsonld/swldInit.h"                       // SWLD_CORE_CONTEXT_URL
#include "swJsonld/swldDownload.h"                   // swldContextFromUrl
#include "swJsonld/swldCompactTree.h"                // swldCompactTreeWith

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdOp.h"                           // LdOpMergeEntity, LdOpBatchMerge
#include "swNgsild/LdNormalizeInput.h"               // ldNormalizeInput
#include "swNgsild/ldCheckEntity.h"                  // ldCheckEntity
#include "swNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "swNgsild/ldStripAtContext.h"              // ldStripAtContext
#include "swNgsild/LdProblem.h"                      // LD_ERROR_RESOURCE_NOT_FOUND, LD_ERROR_CONFLICT, LD_ERROR_INTERNAL_ERROR
#include "swNgsild/ldEntityMerge.h"                  // LdMergeReport
#include "swNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityUpdate
#include "swNgsild/ldNotifyDefer.h"                  // ldNotifyDefer

#include "troe/troeFromMerge.h"                      // troeDeferAttrEventsFromMerge
#include "swNgsild/LdSubCache.h"                     // LdSubCache

#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "swNgsild/ldDistOp.h"                       // ldDistOp*
#include "swNgsild/ldEntityFragment.h"               // ldEntityFragmentForInfo

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND, DB_ERR
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postEntityBatchMerge.h"    // Own interface



// -----------------------------------------------------------------------------
//
// addBatchError - append a BatchEntityError (§ 5.2.17) to errors[].
//
static void addBatchError(KjNode* errorsP, const char* entityId,
                          const char* errType, const char* title,
                          const char* detail, const char* regId)
{
  KjNode* err = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(err, kjString(swRest.kjsonP, "entityId", (char*) entityId));

  KjNode* pd = kjObject(swRest.kjsonP, "error");
  kjChildAdd(pd, kjString(swRest.kjsonP, "type",   (char*) errType));
  kjChildAdd(pd, kjString(swRest.kjsonP, "title",  (char*) title));
  kjChildAdd(pd, kjString(swRest.kjsonP, "detail", (char*) detail));
  kjChildAdd(err, pd);

  if (regId != NULL)
    kjChildAdd(err, kjString(swRest.kjsonP, "registrationId", (char*) regId));

  kjChildAdd(errorsP, err);
}



// -----------------------------------------------------------------------------
//
// csrJsonldContext - return the jsonldContext URL for a CSR, or NULL.
//
static const char* csrJsonldContext(LdRegCacheItem* csr)
{
  if (csr == NULL || csr->contextSourceInfoKV == NULL)
    return NULL;
  for (int i = 0; csr->contextSourceInfoKV[i] != NULL; i += 2)
  {
    const char* k = csr->contextSourceInfoKV[i];
    if (k != NULL && strcasecmp(k, "jsonldContext") == 0)
      return csr->contextSourceInfoKV[i + 1];
  }
  return NULL;
}



static char* renderBatchBody(LdRegCacheItem* csr, KjNode* batchArr)
{
  const char* jsonldCtxUrl = csrJsonldContext(csr);

  if (jsonldCtxUrl != NULL)
  {
    SwldContext* targetCtx = swldContextFromUrl(jsonldCtxUrl, &swRest.kalloc);
    for (KjNode* fragP = batchArr->value.firstChildP; fragP != NULL; fragP = fragP->next)
    {
      if (targetCtx != NULL) swldCompactTreeWith(fragP, targetCtx);

      KjNode* atCtx = kjLookup(fragP, "@context");
      if (atCtx != NULL) kjChildRemove(fragP, atCtx);
    }
  }
  else
  {
    for (KjNode* fragP = batchArr->value.firstChildP; fragP != NULL; fragP = fragP->next)
    {
      if (kjLookup(fragP, "@context") == NULL)
        kjChildAdd(fragP, kjString(swRest.kjsonP, "@context", SWLD_CORE_CONTEXT_URL));
    }
  }

  int   bufSize = kjFastRenderSize(batchArr) + 1;
  char* buf     = (char*) kaAlloc(&swRest.kalloc, bufSize);
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
  char*       url     = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + 1);
  strcpy(url, csr->endpoint);
  strcpy(url + baseLen, path);

  char* body    = renderBatchBody(csr, batchArr);
  int   bodyLen = strlen(body);

  char*       respBody    = NULL;
  int         respBodyLen = 0;
  const char* upErr       = NULL;

  int status = ldDistOpSendReceive(csr, SwVerbPost, url, body, bodyLen,
                                    ownAlias, &upErr,
                                    &respBody, &respBodyLen);

  if (respBody != NULL && respBodyLen > 0)
  {
    KjNode* treeP = kjParse(swRest.kjsonP, respBody);
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
      addBatchError(errorsP, idV[i],
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
      if (errP != NULL && errP->type == KjObject)
      {
        KjNode* tP = kjLookup(errP, "type");
        KjNode* hP = kjLookup(errP, "title");
        KjNode* dP = kjLookup(errP, "detail");
        if (tP != NULL && tP->type == KjString) type   = tP->value.s;
        if (hP != NULL && hP->type == KjString) title  = hP->value.s;
        if (dP != NULL && dP->type == KjString) detail = dP->value.s;
      }
      addBatchError(errorsP, eid, type, title, detail, csrRegId);
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
    CsrAccum* newV = (CsrAccum*) kaAlloc(&swRest.kalloc, newCap * sizeof(CsrAccum));
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
    KjNode**     newF  = (KjNode**)     kaAlloc(&swRest.kalloc, newCap * sizeof(KjNode*));
    const char** newId = (const char**) kaAlloc(&swRest.kalloc, newCap * sizeof(char*));
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
      KjNode* chopped = ldEntityFragmentForInfo(fragP, riP, swRest.kjsonP, detach);
      if (chopped == NULL)
        continue;

      CsrAccum* a = csrAccumFindOrCreate(aVp, aNp, aCapP, csr, mode);
      csrAccumAppend(a, chopped, entityId);
    }
  }

  if (matchV != NULL) free(matchV);
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
  KjNode* bodyP = swRest.in.requestTree;

  if (bodyP->type != KjArray)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Batch Entity Merge body must be a JSON array");
    return true;
  }

  int total = 0;
  for (KjNode* c = bodyP->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->type == KjNull)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "Batch Entity Merge: null entry at position %d", total);
      return true;
    }
    total++;
  }

  if (total == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Batch Entity Merge: input array is empty");
    return true;
  }

  KjNode* successP = kjArray(swRest.kjsonP, "success");
  KjNode* errorsP  = kjArray(swRest.kjsonP, "errors");

  //
  // Pass 1 — validate + extract id per fragment. We keep the array order
  // and the fragment list flat; same-id duplicates are applied in order
  // by the DB plugin.
  //
  KjNode**     fragV   = (KjNode**)     kaAlloc(&swRest.kalloc, sizeof(KjNode*) * total);
  const char** idV     = (const char**) kaAlloc(&swRest.kalloc, sizeof(char*)  * total);
  int          fragN   = 0;

  for (KjNode* inP = bodyP->value.firstChildP; inP != NULL; inP = inP->next)
  {
    if (inP->type != KjObject)
    {
      addBatchError(errorsP, "",
                    LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
                    "entity must be a JSON object", NULL);
      continue;
    }

    ldNormalizeInput(inP, &swRest.kalloc, false);

    if (ldCheckEntity(inP, LdOpBatchMerge, NULL, &swRest.kalloc) == false)
    {
      const char* eid = "";
      KjNode* idP = kjLookup(inP, "id");
      if (idP != NULL && idP->type == KjString) eid = idP->value.s;

      char snapshot[512];
      strncpy(snapshot, swRest.out.problemDetail, sizeof(snapshot) - 1);
      snapshot[sizeof(snapshot) - 1] = 0;

      addBatchError(errorsP, eid,
                    LD_ERROR_BAD_REQUEST_DATA, "Bad Request", snapshot, NULL);

      swRest.out.httpStatusCode   = 0;
      swRest.out.problemType      = NULL;
      swRest.out.problemTitle     = NULL;
      swRest.out.problemDetail[0] = 0;
      continue;
    }

    KjNode* idP = kjLookup(inP, "id");
    if (idP == NULL || idP->type != KjString)
    {
      addBatchError(errorsP, "",
                    LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
                    "entity id is missing or not a string", NULL);
      continue;
    }

    fragV[fragN] = inP;
    idV[fragN]   = idP->value.s;
    fragN++;
  }

  if (fragN == 0)
  {
    KjNode* respBodyP = kjObject(swRest.kjsonP, NULL);
    kjChildAdd(respBodyP, successP);
    kjChildAdd(respBodyP, errorsP);
    swRest.out.responseTree   = respBodyP;
    swRest.out.httpStatusCode = 400;
    swNgsild.rawResponse      = true;
    return true;
  }

  //
  // Pass 2 — distops chop per fragment in array order. Build the flat
  // localFragsArr of fragments whose non-keyword attrs survived the
  // chop and therefore need a local merge.
  //
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  LdSubCache* subCacheP = (LdSubCache*) tenantP->subCacheP;

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

  bool dispatch = (swNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;

  CsrAccum* csrAccums    = NULL;
  int       csrAccumsN   = 0;
  int       csrAccumsCap = 0;

  KjNode*      localFragsArr = kjArray(swRest.kjsonP, NULL);
  const char** localIdV      = (const char**) kaAlloc(&swRest.kalloc, sizeof(char*) * fragN);
  int          localN        = 0;

  // anySuccessV: per unique id flag — true if any local or distop succeeded for that id.
  // Unique ids live in uniqueIdV; uniqueIdN is the count.
  const char** uniqueIdV  = (const char**) kaAlloc(&swRest.kalloc, sizeof(char*) * fragN);
  bool*        anySuccessV = (bool*)        kaAlloc(&swRest.kalloc, sizeof(bool)  * fragN);
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

      chopForMode(tenantP, id, typeArr, scopeV, fragP,
                  LdRegModeExclusive, true,
                  &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
      chopForMode(tenantP, id, typeArr, scopeV, fragP,
                  LdRegModeRedirect, true,
                  &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
      chopForMode(tenantP, id, typeArr, scopeV, fragP,
                  LdRegModeInclusive, false,
                  &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
    }

    if (!hasAnyNonKeywordAttr(fragP))
      continue;

    ldApiEntityToDbModel(fragP, &swRest.kalloc);

    kjChildAdd(localFragsArr, fragP);
    localIdV[localN++] = id;
  }

  //
  // Pass 3 — synchronous distops forward per CSR.
  //
  for (int ai = 0; ai < csrAccumsN; ai++)
  {
    CsrAccum*       a   = &csrAccums[ai];
    LdRegCacheItem* csr = a->csr;

    if (a->count == 0)
      continue;

    if (!ldRegOpSupported(csr, LdOpBatchMerge))
    {
      if (a->mode == LdRegModeExclusive || a->mode == LdRegModeRedirect)
      {
        const char* detail = (a->mode == LdRegModeExclusive)
                             ? "exclusive registration does not support mergeBatch"
                             : "redirect registration does not support mergeBatch";
        for (int i = 0; i < a->count; i++)
          addBatchError(errorsP, a->idV[i],
                        LD_ERROR_CONFLICT, "Conflict", detail, csr->regId);
      }
      continue;
    }

    KjNode* batchArr = kjArray(swRest.kjsonP, NULL);
    for (int i = 0; i < a->count; i++)
      kjChildAdd(batchArr, a->fragV[i]);

    KjNode* respTreeP = NULL;
    int     status    = forwardBatchToCSR(csr, batchArr, ownAlias, &respTreeP);

    bool* groupOk = (bool*) kaAlloc(&swRest.kalloc, sizeof(bool) * a->count);
    for (int k = 0; k < a->count; k++) groupOk[k] = false;

    applyRemoteBatchResult(status, respTreeP, csr->regId, errorsP,
                           groupOk, a->idV, a->count);

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

  //
  // Pass 4 — bulk DB merge. The plugin loops per fragment in array order,
  // applying each surgical merge. Returns per-fragment result + report +
  // post-merge snapshot.
  //
  if (localN > 0)
  {
    if (db.entityBulkMerge == NULL)
    {
      ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
              "Batch Entity Merge not supported by this DB plugin");
      return true;
    }

    int*            resultsV    = (int*)            kaAlloc(&swRest.kalloc, sizeof(int)             * localN);
    LdMergeReport*  reportsV    = (LdMergeReport*)  kaAlloc(&swRest.kalloc, sizeof(LdMergeReport)   * localN);
    KjNode**        snapshotsV  = (KjNode**)        kaAlloc(&swRest.kalloc, sizeof(KjNode*)         * localN);

    for (int k = 0; k < localN; k++)
    {
      reportsV[k].changes = NULL;
      snapshotsV[k]       = NULL;
    }

    db.entityBulkMerge(tenantP, localFragsArr, swRest.requestStartTime,
                       resultsV, reportsV, snapshotsV);

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
                                         swRest.requestStartTime);
          }
          break;
        }
        case DB_NOT_FOUND:
          addBatchError(errorsP, eid,
                        LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                        "entity does not exist", NULL);
          break;
        default:
          addBatchError(errorsP, eid,
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
    kjChildAdd(successP, kjString(swRest.kjsonP, NULL, (char*) uniqueIdV[ui]));
    successCount++;
  }

  int errorCount = 0;
  for (KjNode* p = errorsP->value.firstChildP; p != NULL; p = p->next) errorCount++;

  if (errorCount == 0 && successCount > 0)
  {
    swRest.out.httpStatusCode = 204;
    return true;
  }

  KjNode* respBodyP = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(respBodyP, successP);
  kjChildAdd(respBodyP, errorsP);
  swRest.out.responseTree   = respBodyP;
  swRest.out.httpStatusCode = (successCount > 0) ? 207 : 409;
  swNgsild.rawResponse      = true;
  return true;
}

//
// FILE            postEntityBatchCreate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/entityOperations/create — Batch Entity Creation (§ 5.6.7).
//
// Flow (§ 5.6.7.4):
//
//   Pass 1 — validation + normalisation + § 5.5.11.1 first-wins dedup.
//            Produces `eligibleP`, a KjArray of API-form entities that
//            passed per-entity validation. `eligIdV[i]` holds the id of
//            eligibleP's i-th child.
//
//   Pass 2 — distops forwarding. For each mode (exclusive, redirect,
//            inclusive) we group matching entities by CSR and forward
//            all of them in a single POST /entityOperations/create per
//            CSR — one HTTP round-trip per CSR regardless of batch size.
//            For exclusive/redirect the claimed attrs are chopped off
//            the local entity (the broker must not hold them);
//            inclusive leaves the local entity intact.
//
//   Pass 3 — bulk local create on whatever remains in eligibleP. Uses
//            db.entityBulkCreate → one insert_many for mongoc.
//
//   Pass 4 — response assembly. A BatchOperationResult (§ 5.2.17):
//            201 Created all-OK, 207 Multi-Status partial, 409 Conflict
//            none-succeeded.
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp, strncpy, strlen, strcpy
#include <stdlib.h>                                  // free
#include <stdio.h>                                   // snprintf

#include "swRest/SwRestState.h"                      // swRest
#include "swRest/SwRestVerb.h"                       // SwVerbPost

#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjArray, kjString, kjChildAdd
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjParse.h"                           // kjParse
#include "kjson/kjRender.h"                          // kjFastRender
#include "kjson/kjRenderSize.h"                      // kjFastRenderSize

#include "swJsonld/swldInit.h"                       // SWLD_CORE_CONTEXT_URL
#include "swJsonld/swldDownload.h"                   // swldContextFromUrl
#include "swJsonld/swldCompactTree.h"                // swldCompactTreeWith
#include "kjson/kjBuilder.h"                         // kjChildRemove — already transitively pulled by the earlier include

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdOp.h"                           // LdOpCreateEntity
#include "swNgsild/LdNormalizeInput.h"               // ldNormalizeInput
#include "swNgsild/ldCheckEntity.h"                  // ldCheckEntity
#include "swNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "swNgsild/ldStripAtContext.h"              // ldStripAtContext
#include "swNgsild/LdProblem.h"                      // LD_ERROR_ALREADY_EXISTS, LD_ERROR_CONFLICT

#include "swNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityCreate
#include "swNgsild/ldNotifyDefer.h"                  // ldNotifyDefer

#include "troe/TroeDriver.h"                         // TroeEvent, TroeOpEntityCreated
#include "troe/troeDispatch.h"                       // troeDeferEntityEvent
#include "swNgsild/LdSubCache.h"                     // LdSubCache

#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode, LdRegInfo
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "swNgsild/ldDistOp.h"                       // ldDistOp*
#include "swNgsild/ldEntityFragment.h"               // ldEntityFragmentForInfo

#include "db/DbDriver.h"                             // db, DB_OK, DB_ALREADY_EXISTS, DB_ERR
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postEntityBatchCreate.h"   // Own interface



// -----------------------------------------------------------------------------
//
// addBatchError - append a BatchEntityError (§ 5.2.17) to errors[].
//
static void addBatchError(KjNode* errorsP, const char* entityId, int statusCode,
                          const char* errType, const char* title,
                          const char* detail, const char* regId)
{
  KjNode* err = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(err, kjString(swRest.kjsonP, "entityId", (char*) entityId));

  KjNode* pd = kjObject(swRest.kjsonP, "error");
  kjChildAdd(pd, kjString (swRest.kjsonP, "type",   (char*) errType));
  kjChildAdd(pd, kjString (swRest.kjsonP, "title",  (char*) title));
  kjChildAdd(pd, kjInteger(swRest.kjsonP, "status", statusCode));
  kjChildAdd(pd, kjString (swRest.kjsonP, "detail", (char*) detail));
  kjChildAdd(err, pd);

  if (regId != NULL)
    kjChildAdd(err, kjString(swRest.kjsonP, "registrationId", (char*) regId));

  kjChildAdd(errorsP, err);
}



// -----------------------------------------------------------------------------
//
// entityInfoCoversId - does any EntityInfo in riP cover this entity id?
//
static bool entityInfoCoversId(LdRegInfo* riP, const char* entityId)
{
  for (LdRegEntityInfo* eiP = riP->entityInfoV; eiP != NULL; eiP = eiP->next)
  {
    if (eiP->id == NULL && eiP->idPatternList == NULL) return true;
    if (eiP->id != NULL && strcmp(eiP->id, entityId) == 0) return true;
    // idPattern: the cache already decided this csr matches. Accept.
    if (eiP->idPatternList != NULL) return true;
  }
  return false;
}



// -----------------------------------------------------------------------------
//
// entityAtIndex - O(i) walk of eligibleP to return its i-th child.
//
// Used by the distops dispatcher when it needs to re-locate an entity
// after recording (group, idx) during the per-entity match pass. A
// linear scan is fine for moderate batches; large ones can be pre-
// indexed if the profiles show it.
//
static KjNode* entityAtIndex(KjNode* eligibleP, int idx)
{
  int i = 0;
  for (KjNode* e = eligibleP->value.firstChildP; e != NULL; e = e->next, i++)
    if (i == idx) return e;
  return NULL;
}



// -----------------------------------------------------------------------------
//
// typeVecOf - build a temporary char*[] for an entity's "type" field.
//
static char** typeVecOf(KjNode* entityP, char* slot[2])
{
  slot[0] = NULL;
  slot[1] = NULL;
  KjNode* tP = kjLookup(entityP, "type");
  if (tP == NULL || tP->type != KjString) return NULL;
  slot[0] = tP->value.s;
  return slot;
}



// -----------------------------------------------------------------------------
//
// renderBatchBody - serialise the KjArray of fragments for forwarding.
//
// Two modes per § 4.3.6.6 + § 6.3.19:
//
//   - CSR has no "jsonldContext" in contextSourceInfo:
//       Each fragment gets a per-element @context (core context URL).
//       The request goes out as application/ld+json (per § 6.3.5's
//       per-element ld+json rule — see spec-doubts #15).
//
//   - CSR has "jsonldContext":
//       Broker MUST (§ 4.3.6.6) compact the body against that context
//       AND strip every @context from the body, AND set Content-Type
//       to application/json. The Link header emitted by buildHeaders
//       then carries the context URL. This function handles the body
//       side (compact + strip); the headers are set elsewhere.
//
static char* renderBatchBody(LdRegCacheItem* csr, KjNode* batchArr)
{
  //
  // § 4.3.6.6: compact every fragment against the effective forward
  // context — csi.jsonldContext > incoming request @context > core
  // (ldDistOpForwardContext: the same context buildHeaders names in the
  // Link header) — and strip in-body @context (the forward goes out as
  // application/json + Link).
  //
  SwldContext* fwdCtx = ldDistOpForwardContext(csr);

  for (KjNode* fragP = batchArr->value.firstChildP; fragP != NULL; fragP = fragP->next)
  {
    swldCompactTreeWith(fragP, fwdCtx);

    KjNode* atCtx = kjLookup(fragP, "@context");
    if (atCtx != NULL)
      kjChildRemove(fragP, atCtx);
  }

  int   bufSize = kjFastRenderSize(batchArr) + 1;
  char* buf     = (char*) kaAlloc(&swRest.kalloc, bufSize);
  kjFastRender(batchArr, buf);
  return buf;
}



// -----------------------------------------------------------------------------
//
// forwardBatchToCSR - POST one grouped batch to a CSR's batch-create.
//
static int forwardBatchToCSR(LdRegCacheItem* csr, KjNode* batchArr,
                              const char* ownAlias, KjNode** respTreePP)
{
  *respTreePP = NULL;

  const char* path    = "/ngsi-ld/v1/entityOperations/create";
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



// -----------------------------------------------------------------------------
//
// applyRemoteBatchResult - map a remote broker's BatchOperationResult
// onto per-entity outcomes.
//
//   201 Created (no body)         → every forwarded id succeeded
//   207 Multi-Status with body    → parse success[] + errors[]
//   transport / other failure     → one Bad-Gateway error per forwarded id
//
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
// dispatchOneMode - group matching entities by CSR for one mode and
// forward each group in a single batch POST /entityOperations/create.
//
typedef struct Group
{
  LdRegCacheItem* csr;
  int*            idxV;     // entity indices into eligibleP's child order
  LdRegInfo**     riV;      // matching RegistrationInfo per entry
  int             count;
  int             capacity;
} Group;

static void dispatchOneMode(Tenant*       tenantP,
                             LdRegMode    mode,
                             bool         detach,
                             bool         conflictOnNoOp,
                             KjNode*      eligibleP,
                             const char** idV,
                             int          N,
                             bool*        anySuccessV,
                             KjNode*      errorsP,
                             const char*  ownAlias)
{
  Group* groups = NULL;
  int    gN     = 0;
  int    gCap   = 0;

  int idx = 0;
  for (KjNode* ent = eligibleP->value.firstChildP; ent != NULL; ent = ent->next, idx++)
  {
    KjNode* idP = kjLookup(ent, "id");
    if (idP == NULL || idP->type != KjString) continue;

    char*   slot[2];
    char**  typeArr = typeVecOf(ent, slot);

    KjNode* scopeP       = kjLookup(ent, "scope");
    char*   scopeBuf[2]  = { NULL, NULL };
    char**  scopeV       = NULL;
    if (scopeP != NULL && scopeP->type == KjString)
    {
      scopeBuf[0] = scopeP->value.s;
      scopeV      = scopeBuf;
    }

    LdRegCacheItem** matchV = NULL;
    int matchN = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                   idP->value.s, typeArr, scopeV,
                                                   mode, &matchV);
    for (int m = 0; m < matchN; m++)
    {
      LdRegCacheItem* csr = matchV[m];
      if (csr->endpoint == NULL) continue;
      if (ldDistOpCsrWouldLoop(csr, ownAlias)) continue;

      LdRegInfo* selectedRi = NULL;
      for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
      {
        if (entityInfoCoversId(riP, idP->value.s))
        {
          selectedRi = riP;
          break;
        }
      }
      if (selectedRi == NULL) continue;

      Group* g = NULL;
      for (int k = 0; k < gN; k++)
        if (groups[k].csr == csr) { g = &groups[k]; break; }

      if (g == NULL)
      {
        if (gN >= gCap)
        {
          int newCap = gCap == 0 ? 4 : gCap * 2;
          Group* newGroups = (Group*) kaAlloc(&swRest.kalloc, sizeof(Group) * newCap);
          for (int c = 0; c < gN; c++) newGroups[c] = groups[c];
          groups = newGroups;
          gCap   = newCap;
        }
        groups[gN].csr      = csr;
        groups[gN].idxV     = NULL;
        groups[gN].riV      = NULL;
        groups[gN].count    = 0;
        groups[gN].capacity = 0;
        g = &groups[gN];
        gN++;
      }

      if (g->count >= g->capacity)
      {
        int newCap = g->capacity == 0 ? 4 : g->capacity * 2;
        int*        newIdx = (int*)        kaAlloc(&swRest.kalloc, sizeof(int)        * newCap);
        LdRegInfo** newRi  = (LdRegInfo**) kaAlloc(&swRest.kalloc, sizeof(LdRegInfo*) * newCap);
        for (int c = 0; c < g->count; c++)
        {
          newIdx[c] = g->idxV[c];
          newRi[c]  = g->riV[c];
        }
        g->idxV     = newIdx;
        g->riV      = newRi;
        g->capacity = newCap;
      }
      g->idxV[g->count] = idx;
      g->riV[g->count]  = selectedRi;
      g->count++;
    }

    if (matchV != NULL) free(matchV);
  }

  //
  // Phase 1: build per-CSR batch bodies (skip unsupported-op groups, emit
  // Conflict errors when relevant). Phase 2: ldDistOpSendMulti across all
  // supported groups in parallel. Phase 3: apply each per-CSR result.
  //
  LdDistOpBatchItem*   bItems   = (LdDistOpBatchItem*)   kaAlloc(&swRest.kalloc, gN * sizeof(LdDistOpBatchItem));
  memset(bItems, 0, gN * sizeof(LdDistOpBatchItem));
  LdDistOpBatchResult* bResults = (LdDistOpBatchResult*) kaAlloc(&swRest.kalloc, gN * sizeof(LdDistOpBatchResult));
  const char***        bFwdIdV  = (const char***)        kaAlloc(&swRest.kalloc, gN * sizeof(const char**));
  int**                bFwdOrig = (int**)                kaAlloc(&swRest.kalloc, gN * sizeof(int*));
  int*                 bFwdN    = (int*)                 kaAlloc(&swRest.kalloc, gN * sizeof(int));
  int                  bItemCount = 0;
  memset(bResults, 0, gN * sizeof(LdDistOpBatchResult));

  const char* batchPath = "/ngsi-ld/v1/entityOperations/create";
  int         batchPathLen = strlen(batchPath);

  for (int gi = 0; gi < gN; gi++)
  {
    Group* g = &groups[gi];
    LdRegCacheItem* csr = g->csr;

    if (!ldRegOpSupported(csr, swRest.serviceP->ldOp))
    {
      if (conflictOnNoOp)
      {
        for (int e = 0; e < g->count; e++)
        {
          int i = g->idxV[e];
          KjNode* ent = entityAtIndex(eligibleP, i);
          if (ent != NULL && detach)
            ldEntityFragmentForInfo(ent, g->riV[e], swRest.kjsonP, true);

          char detail[256];
          snprintf(detail, sizeof(detail),
                   "%s registration does not support createEntity",
                   mode == LdRegModeExclusive ? "exclusive" :
                   mode == LdRegModeRedirect  ? "redirect"  : "inclusive");
          addBatchError(errorsP, idV[i], 409,
                        LD_ERROR_CONFLICT, "Conflict", detail, csr->regId);
        }
      }
      continue;
    }

    KjNode*      batchArr      = kjArray(swRest.kjsonP, NULL);
    const char** forwardedIdV  = (const char**) kaAlloc(&swRest.kalloc, sizeof(char*) * g->count);
    int*         forwardedOrigIdx = (int*)      kaAlloc(&swRest.kalloc, sizeof(int)   * g->count);
    int          forwardedN    = 0;

    for (int e = 0; e < g->count; e++)
    {
      int i = g->idxV[e];
      KjNode* ent = entityAtIndex(eligibleP, i);
      if (ent == NULL) continue;

      KjNode* fragP = ldEntityFragmentForInfo(ent, g->riV[e], swRest.kjsonP, detach);
      if (fragP == NULL) continue;

      kjChildAdd(batchArr, fragP);
      forwardedIdV[forwardedN]     = idV[i];
      forwardedOrigIdx[forwardedN] = i;
      forwardedN++;
    }

    if (forwardedN == 0) continue;

    int   baseLen = strlen(csr->endpoint);
    char* url     = (char*) kaAlloc(&swRest.kalloc, baseLen + batchPathLen + 1);
    strcpy(url, csr->endpoint);
    strcpy(url + baseLen, batchPath);

    char* body    = renderBatchBody(csr, batchArr);
    int   bodyLen = strlen(body);

    bItems[bItemCount].csr     = csr;
    bItems[bItemCount].url     = url;
    bItems[bItemCount].body    = body;
    bItems[bItemCount].bodyLen = bodyLen;
    bFwdIdV[bItemCount]        = forwardedIdV;
    bFwdOrig[bItemCount]       = forwardedOrigIdx;
    bFwdN[bItemCount]          = forwardedN;
    bItemCount++;
  }

  if (bItemCount > 0)
  {
    ldDistOpSendMulti(bItems, bItemCount, SwVerbPost, ownAlias, bResults);

    for (int i = 0; i < bItemCount; i++)
    {
      KjNode* respTreeP = NULL;
      if (bResults[i].responseBody != NULL && bResults[i].responseBodyLen > 0)
      {
        KjNode* treeP = kjParse(swRest.kjsonP, bResults[i].responseBody);
        if (treeP != NULL)
        {
          ldStripAtContext(treeP);
          respTreeP = treeP;
        }
      }

      bool* groupOk = (bool*) kaAlloc(&swRest.kalloc, sizeof(bool) * bFwdN[i]);
      for (int k = 0; k < bFwdN[i]; k++) groupOk[k] = false;

      applyRemoteBatchResult(bResults[i].statusCode, respTreeP, bItems[i].csr->regId, errorsP,
                             groupOk, bFwdIdV[i], bFwdN[i]);

      for (int k = 0; k < bFwdN[i]; k++)
        if (groupOk[k])
          anySuccessV[bFwdOrig[i][k]] = true;
    }
  }
}



// -----------------------------------------------------------------------------
//
// hasNonKeywordAttr - true if the entity still has at least one attr
// after distops chopping.
//
static bool hasNonKeywordAttr(KjNode* entityP)
{
  if (entityP == NULL || entityP->type != KjObject) return false;
  for (KjNode* c = entityP->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->name == NULL)                     continue;
    if (c->name[0] == '@')                   continue;
    if (strcmp(c->name, "id")   == 0)        continue;
    if (strcmp(c->name, "type") == 0)        continue;
    return true;
  }
  return false;
}



// -----------------------------------------------------------------------------
//
// postEntityBatchCreate -
//
bool postEntityBatchCreate(void)
{
  KjNode* bodyP = swRest.in.requestTree;

  if (bodyP->type != KjArray)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Batch Entity Creation body must be a JSON array");
    return true;
  }

  int total = 0;
  for (KjNode* c = bodyP->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->type == KjNull)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "Batch Entity Creation: null entry at position %d", total);
      return true;
    }
    total++;
  }

  // batchPreErrors (set by ldParseHook on per-element @context misses) is
  // not "empty body" — it carries entries to surface. Empty-array 400 only
  // applies when nothing arrived AND nothing was pre-rejected.
  bool hasPreErrors = (swNgsild.batchPreErrors != NULL &&
                       swNgsild.batchPreErrors->value.firstChildP != NULL);
  if (total == 0 && !hasPreErrors)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Batch Entity Creation: input array is empty");
    return true;
  }

  KjNode* successP = kjArray(swRest.kjsonP, "success");
  KjNode* errorsP  = kjArray(swRest.kjsonP, "errors");

  // Splice parse-time per-entity errors (e.g. ld+json element missing
  // @context) onto errorsP. errorsP is freshly empty here, so this is O(1).
  if (hasPreErrors)
  {
    errorsP->value.firstChildP = swNgsild.batchPreErrors->value.firstChildP;
    errorsP->lastChild         = swNgsild.batchPreErrors->lastChild;
    swNgsild.batchPreErrors    = NULL;
  }

  //
  // Pass 1 — validate, normalise, dedup.
  //
  KjNode*      eligibleP = kjArray(swRest.kjsonP, NULL);
  const char** eligIdV   = (const char**) kaAlloc(&swRest.kalloc, sizeof(char*) * total);
  bool*        hadAttrsV = (bool*)        kaAlloc(&swRest.kalloc, sizeof(bool)  * total);
  int          eligN     = 0;

  KjNode* seen = kjObject(swRest.kjsonP, NULL);

  KjNode* inP = bodyP->value.firstChildP;
  while (inP != NULL)
  {
    KjNode* nextP = inP->next;

    if (inP->type != KjObject)
    {
      addBatchError(errorsP, "", 400,
                    LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
                    "entity must be a JSON object", NULL);
      inP = nextP;
      continue;
    }

    ldNormalizeInput(inP, &swRest.kalloc, false);

    if (ldCheckEntity(inP, LdOpCreateEntity, NULL, &swRest.kalloc) == false)
    {
      const char* eid = "";
      KjNode* idP = kjLookup(inP, "id");
      if (idP != NULL && idP->type == KjString) eid = idP->value.s;

      char snapshot[512];
      strncpy(snapshot, swRest.out.problemDetail, sizeof(snapshot) - 1);
      snapshot[sizeof(snapshot) - 1] = 0;

      addBatchError(errorsP, eid, 400,
                    LD_ERROR_BAD_REQUEST_DATA, "Bad Request", snapshot, NULL);

      swRest.out.httpStatusCode   = 0;
      swRest.out.problemType      = NULL;
      swRest.out.problemTitle     = NULL;
      swRest.out.problemDetail[0] = 0;

      inP = nextP;
      continue;
    }

    KjNode* idP = kjLookup(inP, "id");
    if (idP == NULL || idP->type != KjString)
    {
      addBatchError(errorsP, "", 400,
                    LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
                    "entity id is missing or not a string", NULL);
      inP = nextP;
      continue;
    }

    //
    // § 9.3.3 guard — a ?local=true write must not produce local data that
    // an exclusive or redirect registration claims.
    //
    if (swNgsild.local == true && ((Tenant*) swNgsild.tenantP)->regCacheP != NULL)
    {
      const char* cRegId = ldRegCacheLocalWriteConflictTree((LdRegCache*) ((Tenant*) swNgsild.tenantP)->regCacheP,
                                                            idP->value.s, inP, &swRest.kalloc);
      if (cRegId != NULL)
      {
        addBatchError(errorsP, idP->value.s, 409,
                      LD_ERROR_ALREADY_EXISTS, "Conflict",
                      "local write overlaps with registration (§ 9.3.3 — no local data for an exclusive/redirect scope)",
                      cRegId);
        inP = nextP;
        continue;
      }
    }

    const char* eid = idP->value.s;

    if (kjLookup(seen, eid) != NULL)
    {
      addBatchError(errorsP, eid, 409,
                    LD_ERROR_ALREADY_EXISTS, "Already Exists",
                    "duplicate entity id within the same batch", NULL);
      inP = nextP;
      continue;
    }
    kjChildAdd(seen, kjString(swRest.kjsonP, eid, ""));

    eligIdV[eligN]   = eid;
    hadAttrsV[eligN] = hasNonKeywordAttr(inP);
    eligN++;
    kjChildAdd(eligibleP, inP);

    inP = nextP;
  }

  if (eligN == 0)
  {
    int singleStatus = ldBatchErrorsSingleStatus(errorsP);
    if (singleStatus > 0)
    {
      swRest.out.responseTree   = ldBatchErrorAsProblemDetails(errorsP);
      swRest.out.httpStatusCode = singleStatus;
    }
    else
    {
      KjNode* respBodyP = kjObject(swRest.kjsonP, NULL);
      kjChildAdd(respBodyP, successP);
      kjChildAdd(respBodyP, errorsP);
      swRest.out.responseTree   = respBodyP;
      swRest.out.httpStatusCode = 207;
      swNgsild.rawResponse      = true;
    }
    return true;
  }

  //
  // Pass 2 — distops forwarding (§ 5.6.7.4). Group-by-CSR + one batch
  // forward per CSR.
  //
  bool* anySuccessV = (bool*) kaAlloc(&swRest.kalloc, sizeof(bool) * eligN);
  for (int i = 0; i < eligN; i++) anySuccessV[i] = false;

  Tenant*     tenantP  = (Tenant*) swNgsild.tenantP;
  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

  bool dispatch = (swNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;

  if (dispatch)
  {
    // Exclusive: detach in-loop (each excl CSR uniquely owns its
    // attrs). Redirect: clone (multiple redirect CSRs may cover
    // the same entity, all must receive a copy), then iterate
    // matched redirect CSRs once more with detach=true so the
    // local create below only sees what's left. Inclusive: clone.
    dispatchOneMode(tenantP, LdRegModeExclusive, /*detach=*/true,  /*conflictOnNoOp=*/true,
                    eligibleP, eligIdV, eligN, anySuccessV, errorsP, ownAlias);
    dispatchOneMode(tenantP, LdRegModeRedirect,  /*detach=*/false, /*conflictOnNoOp=*/true,
                    eligibleP, eligIdV, eligN, anySuccessV, errorsP, ownAlias);

    // Post-redirect detach sweep: strip redirect-claimed attrs
    // from every entity in eligibleP.
    for (KjNode* ent = eligibleP->value.firstChildP; ent != NULL; ent = ent->next)
    {
      KjNode* idP = kjLookup(ent, "id");
      if (idP == NULL || idP->type != KjString) continue;
      char*   slot[2];
      char**  typeArr     = typeVecOf(ent, slot);
      KjNode* scopeP      = kjLookup(ent, "scope");
      char*   scopeBuf[2] = { NULL, NULL };
      char**  scopeV      = NULL;
      if (scopeP != NULL && scopeP->type == KjString)
      {
        scopeBuf[0] = scopeP->value.s;
        scopeV      = scopeBuf;
      }
      LdRegCacheItem** matchV = NULL;
      int matchN = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                     idP->value.s, typeArr, scopeV,
                                                     LdRegModeRedirect, &matchV);
      for (int m = 0; m < matchN; m++)
      {
        LdRegCacheItem* csr = matchV[m];
        if (csr->endpoint == NULL)              continue;
        if (ldDistOpCsrWouldLoop(csr, ownAlias)) continue;
        for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
        {
          if (!entityInfoCoversId(riP, idP->value.s)) continue;
          (void) ldEntityFragmentForInfo(ent, riP, swRest.kjsonP, /*detach=*/true);
        }
      }
      if (matchV != NULL) free(matchV);
    }

    dispatchOneMode(tenantP, LdRegModeInclusive, /*detach=*/false, /*conflictOnNoOp=*/false,
                    eligibleP, eligIdV, eligN, anySuccessV, errorsP, ownAlias);
  }

  //
  // Pass 3 — bulk local create. Entities fully consumed by exclusive/
  // redirect chopping skip the local store.
  //
  KjNode* localArr  = kjArray(swRest.kjsonP, NULL);
  int*    localIdxV = (int*) kaAlloc(&swRest.kalloc, sizeof(int) * eligN);
  int     localN    = 0;

  {
    //
    // An entity is eligible for local create unless distops consumed
    // every attribute — matching the single-POST path in
    // postEntities.c line 682: `inputHadAttrs && !hasNonKeywordAttr`.
    // An entity that arrived without attrs (pure {id,type} shell)
    // stays local-eligible so its shell is stored.
    //
    int i = 0;
    for (KjNode* ent = eligibleP->value.firstChildP; ent != NULL; ent = ent->next, i++)
    {
      bool distopsAteAll = hadAttrsV[i] && !hasNonKeywordAttr(ent);
      if (!distopsAteAll)
        localIdxV[localN++] = i;
    }

    // Second scan moves those entities into localArr. Use the index
    // vector to avoid mutating eligibleP mid-iteration.
    int cursor = 0;
    int pos    = 0;
    KjNode* ent = eligibleP->value.firstChildP;
    while (ent != NULL)
    {
      KjNode* nextEnt = ent->next;
      if (cursor < localN && localIdxV[cursor] == pos)
      {
        ldApiEntityToDbModel(ent, &swRest.kalloc);
        kjChildAdd(localArr, ent);
        cursor++;
      }
      ent = nextEnt;
      pos++;
    }
  }

  if (localN > 0)
  {
    if (db.entityBulkCreate == NULL)
    {
      ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
              "Batch Entity Creation not supported by this DB plugin");
      return true;
    }

    int* resultsV = (int*) kaAlloc(&swRest.kalloc, sizeof(int) * localN);
    db.entityBulkCreate(tenantP, localArr, resultsV);

    LdSubCache* subCacheP = (LdSubCache*) tenantP->subCacheP;

    KjNode* entP = localArr->value.firstChildP;
    for (int k = 0; k < localN; k++, entP = (entP != NULL) ? entP->next : NULL)
    {
      int origIdx = localIdxV[k];
      const char* eid = eligIdV[origIdx];
      switch (resultsV[k])
      {
        case DB_OK:
          anySuccessV[origIdx] = true;
          if (subCacheP != NULL && entP != NULL)
            ldNotifyDefer(subCacheP, entP, LdNotifyEntityCreate, NULL);

          // TRoE: defer one entity-level created event per successful entity.
          if (entP != NULL)
          {
            KjNode* tn = kjLookup(entP, "type");
            const char* etype = (tn != NULL && tn->type == KjString) ? tn->value.s : NULL;
            TroeEvent* tevP = (TroeEvent*) kaAlloc(&swRest.kalloc, sizeof(TroeEvent));
            memset(tevP, 0, sizeof(*tevP));
            tevP->op             = TroeOpEntityCreated;
            tevP->tenantP        = tenantP;
            tevP->entityId       = eid;
            tevP->entityType     = etype;
            tevP->modifiedAtNs   = swRest.requestStartTime;
            tevP->entitySnapshot = entP;
            troeDeferEntityEvent(tevP);
          }
          break;
        case DB_ALREADY_EXISTS:
          addBatchError(errorsP, eid, 409,
                        LD_ERROR_ALREADY_EXISTS, "Already Exists",
                        "entity already exists", NULL);
          break;
        default:
          addBatchError(errorsP, eid, 500,
                        LD_ERROR_INTERNAL_ERROR, "Internal Error",
                        "database error during batch insert", NULL);
          break;
      }
    }
  }

  //
  // Pass 4 — response assembly.
  //
  for (int i = 0; i < eligN; i++)
  {
    if (anySuccessV[i])
      kjChildAdd(successP, kjString(swRest.kjsonP, NULL, (char*) eligIdV[i]));
  }

  int successCount = 0;
  for (KjNode* p = successP->value.firstChildP; p != NULL; p = p->next) successCount++;

  int errorCount = 0;
  for (KjNode* p = errorsP->value.firstChildP; p != NULL; p = p->next) errorCount++;

  // § 6.14.3.1 — Batch Entity Creation response shape:
  //   201 Created: body is String[] (URIs of successfully-created entities).
  //   207 Multi-Status: body is BatchOperationResult { success, errors }.
  //   All-failed: collapse to plain status when uniform (e.g. all-409 from
  //   AlreadyExists), otherwise 207 with the multi-status envelope.
  if (errorCount == 0)
  {
    successP->name = NULL;                    // unwrap into a top-level array
    swRest.out.responseTree   = successP;
    swRest.out.httpStatusCode = 201;
    swNgsild.rawResponse      = true;
    return true;
  }

  int singleStatus = (successCount == 0) ? ldBatchErrorsSingleStatus(errorsP) : -1;
  if (singleStatus > 0)
  {
    swRest.out.responseTree   = ldBatchErrorAsProblemDetails(errorsP);
    swRest.out.httpStatusCode = singleStatus;
  }
  else
  {
    KjNode* respBodyP = kjObject(swRest.kjsonP, NULL);
    kjChildAdd(respBodyP, successP);
    kjChildAdd(respBodyP, errorsP);
    swRest.out.responseTree   = respBodyP;
    swRest.out.httpStatusCode = 207;
    swNgsild.rawResponse      = true;
  }
  return true;
}

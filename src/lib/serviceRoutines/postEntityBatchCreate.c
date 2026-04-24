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
  const char* jsonldCtxUrl = csrJsonldContext(csr);

  if (jsonldCtxUrl != NULL)
  {
    //
    // § 4.3.6.6: compact body against the referenced context, strip
    // @context members. Context is loaded on demand — cached by
    // swldContextFromUrl so subsequent forwards to the same CSR don't
    // re-download.
    //
    SwldContext* targetCtx = swldContextFromUrl(jsonldCtxUrl, &swRest.kalloc);

    if (targetCtx != NULL)
    {
      for (KjNode* fragP = batchArr->value.firstChildP; fragP != NULL; fragP = fragP->next)
      {
        swldCompactTreeWith(fragP, targetCtx);

        KjNode* atCtx = kjLookup(fragP, "@context");
        if (atCtx != NULL)
          kjChildRemove(fragP, atCtx);
      }
    }
    // If targetCtx failed to load, we still strip any in-body @context
    // (application/json + Link is about to go out, so @context in the
    // body would trigger the receiver's "Unexpected @context" check).
    else
    {
      for (KjNode* fragP = batchArr->value.firstChildP; fragP != NULL; fragP = fragP->next)
      {
        KjNode* atCtx = kjLookup(fragP, "@context");
        if (atCtx != NULL)
          kjChildRemove(fragP, atCtx);
      }
    }
  }
  else
  {
    //
    // Default path: per-element @context for ld+json array body.
    //
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
  // Per CSR: build and forward one batch.
  //
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
          addBatchError(errorsP, idV[i],
                        LD_ERROR_CONFLICT, "Conflict", detail, csr->regId);
        }
      }
      continue;
    }

    KjNode* batchArr = kjArray(swRest.kjsonP, NULL);
    const char** forwardedIdV = (const char**) kaAlloc(&swRest.kalloc, sizeof(char*) * g->count);
    int*         forwardedOrigIdx = (int*)   kaAlloc(&swRest.kalloc, sizeof(int)   * g->count);
    int          forwardedN = 0;

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

    KjNode* respTreeP = NULL;
    int     status    = forwardBatchToCSR(csr, batchArr, ownAlias, &respTreeP);

    bool* groupOk = (bool*) kaAlloc(&swRest.kalloc, sizeof(bool) * forwardedN);
    for (int k = 0; k < forwardedN; k++) groupOk[k] = false;

    applyRemoteBatchResult(status, respTreeP, csr->regId, errorsP,
                           groupOk, forwardedIdV, forwardedN);

    for (int k = 0; k < forwardedN; k++)
      if (groupOk[k])
        anySuccessV[forwardedOrigIdx[k]] = true;
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
  if (swNgsild.contextError)
    return true;

  KjNode* bodyP = swRest.in.requestTree;

  if (swRest.in.payload != NULL && bodyP == NULL)
  {
    ldError(415, LD_ERROR_INVALID_REQUEST, "Unsupported Media Type",
            "supported Content-Types: application/json, application/ld+json");
    return true;
  }

  if (bodyP == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "no payload");
    return true;
  }

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

  if (total == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Batch Entity Creation: input array is empty");
    return true;
  }

  KjNode* successP = kjArray(swRest.kjsonP, "success");
  KjNode* errorsP  = kjArray(swRest.kjsonP, "errors");

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
      addBatchError(errorsP, "",
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

      addBatchError(errorsP, eid,
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
      addBatchError(errorsP, "",
                    LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
                    "entity id is missing or not a string", NULL);
      inP = nextP;
      continue;
    }

    const char* eid = idP->value.s;

    if (kjLookup(seen, eid) != NULL)
    {
      addBatchError(errorsP, eid,
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
    KjNode* respBodyP = kjObject(swRest.kjsonP, NULL);
    kjChildAdd(respBodyP, successP);
    kjChildAdd(respBodyP, errorsP);
    swRest.out.responseTree   = respBodyP;
    swRest.out.httpStatusCode = 409;
    swNgsild.rawResponse      = true;
    return true;
  }

  //
  // Pass 2 — distops forwarding (§ 5.6.7.4). Group-by-CSR + one batch
  // forward per CSR.
  //
  bool* anySuccessV = (bool*) kaAlloc(&swRest.kalloc, sizeof(bool) * eligN);
  for (int i = 0; i < eligN; i++) anySuccessV[i] = false;

  Tenant*     tenantP  = (Tenant*) swNgsild.tenantP;
  const char* ownAlias = (tenantP != NULL)
                         ? ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc)
                         : NULL;

  bool dispatch = (swNgsild.local == false
                   && tenantP != NULL
                   && tenantP->regCacheP != NULL);

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;

  if (dispatch)
  {
    dispatchOneMode(tenantP, LdRegModeExclusive, true,  true,
                    eligibleP, eligIdV, eligN, anySuccessV, errorsP, ownAlias);
    dispatchOneMode(tenantP, LdRegModeRedirect,  true,  true,
                    eligibleP, eligIdV, eligN, anySuccessV, errorsP, ownAlias);
    dispatchOneMode(tenantP, LdRegModeInclusive, false, false,
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
      ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented",
              "Batch Entity Creation not supported by this DB plugin");
      return true;
    }

    int* resultsV = (int*) kaAlloc(&swRest.kalloc, sizeof(int) * localN);
    db.entityBulkCreate(tenantP, localArr, resultsV);

    LdSubCache* subCacheP = (tenantP != NULL) ? (LdSubCache*) tenantP->subCacheP : NULL;

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
          break;
        case DB_ALREADY_EXISTS:
          addBatchError(errorsP, eid,
                        LD_ERROR_ALREADY_EXISTS, "Already Exists",
                        "entity already exists", NULL);
          break;
        default:
          addBatchError(errorsP, eid,
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

  KjNode* respBodyP = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(respBodyP, successP);
  kjChildAdd(respBodyP, errorsP);
  swRest.out.responseTree = respBodyP;

  if (errorCount == 0)
    swRest.out.httpStatusCode = 201;
  else if (successCount > 0)
    swRest.out.httpStatusCode = 207;
  else
    swRest.out.httpStatusCode = 409;

  swNgsild.rawResponse = true;
  return true;
}

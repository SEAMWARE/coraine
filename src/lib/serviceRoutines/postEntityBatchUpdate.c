//
// FILE            postEntityBatchUpdate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/entityOperations/update — Batch Entity Update (§ 5.6.8).
//
// Semantics match Update Attributes (§ 5.6.3) per-entity, extended to a
// list. Multiple instances of the same entity id in one batch are
// **combined in array order** — first element is oldest, last is
// newest — so the merge sequence is deterministic even though every
// instance shares the same wall-clock modifiedAt. Notifications for
// intermediate merged states are queued via ldNotifyDefer and emitted
// post-response as one Notification per subscription with data[]
// carrying every matched state in encounter order.
//
// Flow:
//
//   Pass 1 — validate + group by id (preserving array order within a group).
//
//   Pass 2 — per group: retrieve existing; for each fragment in array
//            order, chop attrs claimed by exclusive/redirect CSRs into
//            per-CSR forwarding queues, clone (non-detaching) for
//            inclusive CSRs, then merge what remains into the in-memory
//            state. Defer one notification candidate per merged state.
//
//   Pass 3 — synchronous distops forward — for each accumulating CSR,
//            one POST /entityOperations/update carrying the ordered
//            array of its chopped/cloned fragments. URL is unchanged
//            from the incoming request (minimal-changes rule). Forward
//            failures populate errors[] with the CSR's registrationId.
//
//   Pass 4 — bulk DB write of the final merged states via
//            db.entityBulkUpdate.
//
//   Pass 5 — response: BatchOperationResult (§ 5.2.17). 204 all-OK,
//            207 partial, 409 all-failed.
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
#include "swNgsild/LdOp.h"                           // LdOpUpdateAttrs, LdOpBatchUpdate
#include "swNgsild/LdNormalizeInput.h"               // ldNormalizeInput
#include "swNgsild/ldCheckEntity.h"                  // ldCheckEntity
#include "swNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "swNgsild/ldStripAtContext.h"              // ldStripAtContext
#include "swNgsild/LdProblem.h"                      // LD_ERROR_RESOURCE_NOT_FOUND, LD_ERROR_CONFLICT, LD_ERROR_INTERNAL_ERROR
#include "swNgsild/ldEntityAttrsSet.h"               // ldEntityAttrsSet
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

#include "serviceRoutines/postEntityBatchUpdate.h"   // Own interface



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



// -----------------------------------------------------------------------------
//
// renderBatchBody - serialise the KjArray of fragments for forwarding.
//
// Two modes per § 4.3.6.6 + § 6.3.19:
//
//   - CSR has no "jsonldContext": per-element @context (core context URL),
//     application/ld+json.
//
//   - CSR has "jsonldContext": compact against that context + strip every
//     in-body @context; Content-Type application/json; Link header (set
//     by the dist-op layer) carries the URL.
//
static char* renderBatchBody(LdRegCacheItem* csr, KjNode* batchArr)
{
  const char* jsonldCtxUrl = csrJsonldContext(csr);

  if (jsonldCtxUrl != NULL)
  {
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
// forwardBatchToCSR - POST one grouped batch to a CSR's /entityOperations/update
//
static int forwardBatchToCSR(LdRegCacheItem* csr, KjNode* batchArr,
                              const char* ownAlias, KjNode** respTreePP)
{
  *respTreePP = NULL;

  const char* path    = "/ngsi-ld/v1/entityOperations/update";
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
// applyRemoteBatchResult - map the remote broker's BatchOperationResult
// to per-id outcomes. 2xx with no body means all forwarded ids OK; 207 with
// body → parse success[] and errors[]; anything else → one Bad-Gateway per
// forwarded id.
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
// Group - fragments targeting one entity id, in arrival order.
//
typedef struct Group
{
  const char*  id;
  KjNode**     fragV;
  int          count;
  int          capacity;
} Group;



// -----------------------------------------------------------------------------
//
// CsrAccum - one entry per CSR that accumulates forwardable fragments.
//
typedef struct CsrAccum
{
  LdRegCacheItem* csr;
  LdRegMode       mode;       // first mode that populated this accum — drives error behaviour
  KjNode**        fragV;      // chopped/cloned fragments, in encounter order
  const char**    idV;        // entity id per fragment (for error reporting)
  int             count;
  int             capacity;
} CsrAccum;



static Group* groupFindOrCreate(Group** groupsP, int* gNp, int* gCapP, const char* id)
{
  for (int i = 0; i < *gNp; i++)
    if (strcmp((*groupsP)[i].id, id) == 0)
      return &(*groupsP)[i];

  if (*gNp >= *gCapP)
  {
    int newCap = (*gCapP == 0) ? 8 : *gCapP * 2;
    Group* newV = (Group*) kaAlloc(&swRest.kalloc, newCap * sizeof(Group));
    for (int i = 0; i < *gNp; i++) newV[i] = (*groupsP)[i];
    *groupsP = newV;
    *gCapP   = newCap;
  }

  (*groupsP)[*gNp].id       = id;
  (*groupsP)[*gNp].fragV    = NULL;
  (*groupsP)[*gNp].count    = 0;
  (*groupsP)[*gNp].capacity = 0;
  return &(*groupsP)[(*gNp)++];
}



static void groupFragAppend(Group* g, KjNode* fragP)
{
  if (g->count >= g->capacity)
  {
    int newCap = (g->capacity == 0) ? 4 : g->capacity * 2;
    KjNode** newV = (KjNode**) kaAlloc(&swRest.kalloc, newCap * sizeof(KjNode*));
    for (int i = 0; i < g->count; i++) newV[i] = g->fragV[i];
    g->fragV    = newV;
    g->capacity = newCap;
  }
  g->fragV[g->count++] = fragP;
}



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



// -----------------------------------------------------------------------------
//
// chopForMode - walk every matching CSR of the given mode for (id, type, scope),
// chop or clone attrs from fragP into the per-CSR accumulator.
//
//   mode == exclusive or redirect → detach=true (attrs leave fragP)
//   mode == inclusive             → detach=false (clone; fragP keeps the attrs)
//
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



// -----------------------------------------------------------------------------
//
// hasAnyNonKeywordAttr - true if fragP still carries at least one attribute
// (i.e. it's not just { id, type, @context } after chopping).
//
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
// postEntityBatchUpdate -
//
bool postEntityBatchUpdate(void)
{
  KjNode* bodyP = swRest.in.requestTree;

  if (bodyP->type != KjArray)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Batch Entity Update body must be a JSON array");
    return true;
  }

  int total = 0;
  for (KjNode* c = bodyP->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->type == KjNull)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "Batch Entity Update: null entry at position %d", total);
      return true;
    }
    total++;
  }

  bool hasPreErrors = (swNgsild.batchPreErrors != NULL &&
                       swNgsild.batchPreErrors->value.firstChildP != NULL);
  if (total == 0 && !hasPreErrors)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Batch Entity Update: input array is empty");
    return true;
  }

  KjNode* successP = kjArray(swRest.kjsonP, "success");
  KjNode* errorsP  = kjArray(swRest.kjsonP, "errors");

  if (hasPreErrors)
  {
    errorsP->value.firstChildP = swNgsild.batchPreErrors->value.firstChildP;
    errorsP->lastChild         = swNgsild.batchPreErrors->lastChild;
    swNgsild.batchPreErrors    = NULL;
  }

  //
  // Pass 1 — validate, normalise, group by id.
  //
  Group* groups = NULL;
  int    gN     = 0;
  int    gCap   = 0;

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

    if (ldCheckEntity(inP, LdOpUpdateAttrs, NULL, &swRest.kalloc) == false)
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

    Group* g = groupFindOrCreate(&groups, &gN, &gCap, idP->value.s);
    groupFragAppend(g, inP);
  }

  if (gN == 0)
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
  // Pass 2 — per group: retrieve, per fragment chop + merge, defer notifs.
  //
  Tenant*      tenantP   = (Tenant*) swNgsild.tenantP;
  LdSubCache*  subCacheP = (LdSubCache*) tenantP->subCacheP;

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

  bool dispatch = (swNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;

  CsrAccum*    csrAccums = NULL;
  int          csrAccumsN   = 0;
  int          csrAccumsCap = 0;

  KjNode*      finals   = kjArray(swRest.kjsonP, NULL);
  const char** finalIdV = (const char**) kaAlloc(&swRest.kalloc, sizeof(char*) * gN);
  bool*        anySuccessV = (bool*) kaAlloc(&swRest.kalloc, sizeof(bool) * gN);
  const char** allIdV   = (const char**) kaAlloc(&swRest.kalloc, sizeof(char*) * gN);
  int          finalN   = 0;

  for (int gi = 0; gi < gN; gi++)
  {
    Group* g  = &groups[gi];
    allIdV[gi]     = g->id;
    anySuccessV[gi] = false;

    KjNode* existingDb = NULL;
    if (db.entityRetrieve == NULL)
    {
      addBatchError(errorsP, g->id,
                    LD_ERROR_INTERNAL_ERROR, "Internal Error",
                    "entityRetrieve not supported by this DB plugin", NULL);
      continue;
    }

    int r = db.entityRetrieve(tenantP, g->id, &existingDb);
    if (r == DB_NOT_FOUND || existingDb == NULL)
    {
      addBatchError(errorsP, g->id,
                    LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                    "entity does not exist", NULL);
      continue;
    }
    if (r != DB_OK)
    {
      addBatchError(errorsP, g->id,
                    LD_ERROR_INTERNAL_ERROR, "Internal Error",
                    "database error during retrieve", NULL);
      continue;
    }

    //
    // Apply each fragment in array order: distops chop → local merge → notify.
    //
    bool anyMerge = false;
    for (int fi = 0; fi < g->count; fi++)
    {
      KjNode* fragP = g->fragV[fi];

      //
      // Distops chop (order matters: exclusive first, then redirect, then
      // inclusive — see § 4.3.6.3).
      //
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

        chopForMode(tenantP, g->id, typeArr, scopeV, fragP,
                    LdRegModeExclusive, true,
                    &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
        chopForMode(tenantP, g->id, typeArr, scopeV, fragP,
                    LdRegModeRedirect, true,
                    &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
        chopForMode(tenantP, g->id, typeArr, scopeV, fragP,
                    LdRegModeInclusive, false,
                    &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
      }

      //
      // Local merge — only if there are attrs left after chopping. The
      // check runs on API-form fragP before ldApiEntityToDbModel injects
      // createdAt/modifiedAt, so a fully-chopped fragment is correctly
      // seen as attr-less.
      //
      if (!hasAnyNonKeywordAttr(fragP))
        continue;

      ldApiEntityToDbModel(fragP, &swRest.kalloc);

      LdMergeReport report = { NULL };
      ldEntityAttrsSet(existingDb, fragP, true /* overwriteScope */,
                       swRest.requestStartTime, &report, swRest.kjsonP);
      anyMerge = true;

      if (subCacheP != NULL)
      {
        KjNode* snapshot = kjClone(swRest.kjsonP, existingDb);
        ldNotifyDefer(subCacheP, snapshot, LdNotifyEntityUpdate, &report);
      }

      // TRoE: per-fragment attr events. Optimistic — fires regardless
      // of the later bulk_update outcome (matches the notification
      // dispatch's pre-existing optimism).
      {
        KjNode* tn = kjLookup(existingDb, "type");
        const char* etype = (tn != NULL && tn->type == KjString) ? tn->value.s : NULL;
        troeDeferAttrEventsFromMerge(tenantP, g->id, etype, existingDb, &report,
                                     swRest.requestStartTime);
      }
    }

    if (anyMerge)
    {
      finalIdV[finalN++] = g->id;
      kjChildAdd(finals, existingDb);
    }
  }

  //
  // Pass 3 — synchronous distops forward, one POST /entityOperations/update
  // per accumulating CSR.
  //
  for (int ai = 0; ai < csrAccumsN; ai++)
  {
    CsrAccum* a   = &csrAccums[ai];
    LdRegCacheItem* csr = a->csr;

    if (a->count == 0)
      continue;

    if (!ldRegOpSupported(csr, LdOpBatchUpdate))
    {
      if (a->mode == LdRegModeExclusive || a->mode == LdRegModeRedirect)
      {
        const char* detail = (a->mode == LdRegModeExclusive)
                             ? "exclusive registration does not support updateBatch"
                             : "redirect registration does not support updateBatch";
        for (int i = 0; i < a->count; i++)
          addBatchError(errorsP, a->idV[i],
                        LD_ERROR_CONFLICT, "Conflict", detail, csr->regId);
      }
      // Inclusive + no updateBatch: silent skip — local already has it.
      continue;
    }

    //
    // Build the KjArray body from the accumulated fragments.
    //
    KjNode* batchArr = kjArray(swRest.kjsonP, NULL);
    for (int i = 0; i < a->count; i++)
      kjChildAdd(batchArr, a->fragV[i]);

    KjNode* respTreeP = NULL;
    int     status    = forwardBatchToCSR(csr, batchArr, ownAlias, &respTreeP);

    bool* groupOk = (bool*) kaAlloc(&swRest.kalloc, sizeof(bool) * a->count);
    for (int k = 0; k < a->count; k++) groupOk[k] = false;

    applyRemoteBatchResult(status, respTreeP, csr->regId, errorsP,
                           groupOk, a->idV, a->count);

    //
    // Map the per-fragment groupOk back to the top-level anySuccessV[gi]
    // by entity id.
    //
    for (int k = 0; k < a->count; k++)
    {
      if (!groupOk[k]) continue;
      for (int gi = 0; gi < gN; gi++)
      {
        if (strcmp(allIdV[gi], a->idV[k]) == 0)
        {
          anySuccessV[gi] = true;
          break;
        }
      }
    }
  }

  //
  // Pass 4 — bulk DB write of final states.
  //
  if (finalN > 0)
  {
    if (db.entityBulkUpdate == NULL)
    {
      ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
              "Batch Entity Update not supported by this DB plugin");
      return true;
    }

    int* resultsV = (int*) kaAlloc(&swRest.kalloc, sizeof(int) * finalN);
    db.entityBulkUpdate(tenantP, finals, resultsV);

    for (int k = 0; k < finalN; k++)
    {
      const char* eid = finalIdV[k];
      switch (resultsV[k])
      {
        case DB_OK:
        {
          // Mark success on the top-level index tracker
          for (int gi = 0; gi < gN; gi++)
            if (strcmp(allIdV[gi], eid) == 0) { anySuccessV[gi] = true; break; }
          break;
        }
        case DB_NOT_FOUND:
          addBatchError(errorsP, eid,
                        LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                        "entity vanished between retrieve and bulk update", NULL);
          break;
        default:
          addBatchError(errorsP, eid,
                        LD_ERROR_INTERNAL_ERROR, "Internal Error",
                        "database error during batch update", NULL);
          break;
      }
    }
  }

  //
  // Pass 5 — response assembly.
  //
  for (int gi = 0; gi < gN; gi++)
  {
    if (anySuccessV[gi])
      kjChildAdd(successP, kjString(swRest.kjsonP, NULL, (char*) allIdV[gi]));
  }

  int successCount = 0;
  for (KjNode* p = successP->value.firstChildP; p != NULL; p = p->next) successCount++;

  int errorCount = 0;
  for (KjNode* p = errorsP->value.firstChildP; p != NULL; p = p->next) errorCount++;

  // § 5.6.10 — batch update returns 204 when there are no errors. The
  // (success=0, errors=0) case (e.g. an empty input array) is success.
  if (errorCount == 0)
  {
    swRest.out.httpStatusCode = 204;
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
    swRest.out.responseTree = respBodyP;
    swRest.out.httpStatusCode = 207;
  }
  swNgsild.rawResponse      = true;
  return true;
}

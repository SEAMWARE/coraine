//
// FILE            postEntityBatchUpsert.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// POST /ngsi-ld/v1/entityOperations/upsert — Batch Entity Upsert (§ 5.6.9).
//
// Per-entity semantics decided by the entity's existence at first
// occurrence:
//
//   - Entity does NOT exist → first fragment CREATES it. Subsequent
//     fragments for the same id always MERGE (not replace), regardless
//     of the request's ?options= mode.
//
//   - Entity exists + ?options=replace (default) → first fragment
//     REPLACES the entity (non-specified attrs disappear). Subsequent
//     fragments MERGE.
//
//   - Entity exists + ?options=update → first fragment MERGES into
//     existing. Subsequent fragments MERGE.
//
// Multi-instance rule per project_batch_multi_instance_ordering: array
// index = time-of-arrival. First element is oldest, last is newest.
// Merges run in array order; the last merged state is what gets
// persisted. Each intermediate state is a notification candidate.
//
// DB flow:
//   - entities that didn't exist at first-occurrence → db.entityBulkCreate.
//   - entities that already existed → db.entityBulkUpdate (replace
//     semantics against the merged final state).
//
// Distops forwarding (§ 5.6.9.4) mirrors Batch Update: per-CSR accum,
// synchronous POST /entityOperations/upsert with the accumulated
// fragments in array order, URL unchanged (minimal-changes rule).
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp, strlen, strncpy, strcasecmp, strcpy
#include <strings.h>                                 // strcasecmp
#include <stdlib.h>                                  // free
#include <stdio.h>                                   // snprintf

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/CorRestVerb.h"                       // CorVerbPost

#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeBuilder.h"                  // corTreeObject, corTreeArray, corTreeString, corTreeChildAdd, corTreeChildRemove
#include "corTree/corTreeClone.h"                    // corTreeClone
#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corJson/corJsonParse.h"                    // corJsonParse
#include "corJson/corJsonRender.h"                   // corJsonFastRender
#include "corJson/corJsonRenderSize.h"               // corJsonFastRenderSize

#include "corJsonld/corLdInit.h"                       // CORLD_CORE_CONTEXT_URL
#include "corJsonld/corLdDownload.h"                   // corLdContextFromUrl
#include "corJsonld/corLdCompactTree.h"                // corLdCompactTreeWith

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdOp.h"                           // LdOpCreateEntity, LdOpUpdateAttrs, LdOpBatchUpsert
#include "corNgsild/LdNormalizeInput.h"               // ldNormalizeInput
#include "corNgsild/ldCheckEntity.h"                  // ldCheckEntity
#include "corNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "corNgsild/ldIsEntityKeyword.h"              // ldIsEntityKeyword
#include "corNgsild/ldStripAtContext.h"              // ldStripAtContext
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*
#include "corNgsild/ldEntityAttrsSet.h"               // ldEntityAttrsSet
#include "corNgsild/ldEntityMerge.h"                  // LdMergeReport
#include "corNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityCreate, LdNotifyEntityUpdate
#include "corNgsild/ldNotifyDefer.h"                  // ldNotifyDefer
#include "bridge/channelCache.h"                      // channelOutCount
#include "bridge/bridgeServiceSync.h"             // bridgeRequestsBeforeWrite, bridgeRequestsWritten, BridgeSyncDone

#include "troe/TroeDriver.h"                         // TroeEvent, TroeOpEntityCreated
#include "troe/troeDispatch.h"                       // troeDeferEntityEvent
#include "troe/troeFromMerge.h"                      // troeDeferAttrEventsFromMerge
#include "corNgsild/LdSubCache.h"                     // LdSubCache

#include "corNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode
#include "corNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "corNgsild/ldDistOp.h"                       // ldDistOp*
#include "corNgsild/ldEntityFragment.h"               // ldEntityFragmentForInfo

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND, DB_ERR, DB_ALREADY_EXISTS
#include "db/Tenant.h"                               // Tenant

#include "corLog/corLog.h"                           // COR_T
#include "coraineTraceLevels.h"                     // KtDistOpRequest

#include "serviceRoutines/postEntityBatchUpsert.h"   // Own interface



// -----------------------------------------------------------------------------
//
// addBatchError - append a BatchEntityError (§ 5.2.17) to errors[].
//
static void addBatchError(CorNode* errorsP, const char* entityId, int statusCode,
                          const char* errType, const char* title,
                          const char* detail, const char* regId)
{
  CorNode* err = corTreeObject(corRest.kallocP, NULL);
  corTreeChildAdd(err, corTreeString(corRest.kallocP, "entityId", (char*) entityId));

  CorNode* pd = corTreeObject(corRest.kallocP, "error");
  corTreeChildAdd(pd, corTreeString (corRest.kallocP, "type", (char*) errType));
  corTreeChildAdd(pd, corTreeString (corRest.kallocP, "title", (char*) title));
  corTreeChildAdd(pd, corTreeInteger(corRest.kallocP, "status", statusCode));
  corTreeChildAdd(pd, corTreeString (corRest.kallocP, "detail", (char*) detail));
  corTreeChildAdd(err, pd);

  if (regId != NULL)
    corTreeChildAdd(err, corTreeString(corRest.kallocP, "registrationId", (char*) regId));

  corTreeChildAdd(errorsP, err);
}



static char* renderBatchBody(LdRegCacheItem* csr, CorNode* batchArr)
{
  //
  // § 4.3.6.6: compact every fragment against the effective forward
  // context — csi.jsonldContext > incoming request @context > core
  // (ldDistOpForwardContext: the same context buildHeaders names in the
  // Link header) — and strip in-body @context (the forward goes out as
  // application/json + Link).
  //
  CorLdContext* fwdCtx = ldDistOpForwardContext(csr);

  for (CorNode* fragP = batchArr->value.head; fragP != NULL; fragP = fragP->next)
  {
    corLdCompactTreeWith(fragP, fwdCtx);

    CorNode* atCtx = corTreeLookup(fragP, "@context");
    if (atCtx != NULL)
      corTreeChildRemove(fragP, atCtx);
  }

  int   bufSize = corJsonFastRenderSize(batchArr) + 1;
  char* buf     = (char*) kaAlloc(&corRest.kalloc, bufSize);
  corJsonFastRender(batchArr, buf);
  return buf;
}



static void applyRemoteBatchResult(int status, CorNode* respTreeP,
                                    const char* csrRegId,
                                    CorNode* errorsP,
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

  CorNode* remoteSuccess = corTreeLookup(respTreeP, "success");
  CorNode* remoteErrors = corTreeLookup(respTreeP, "errors");

  if (remoteSuccess != NULL && remoteSuccess->type == CorArray)
  {
    for (CorNode* sP = remoteSuccess->value.head; sP != NULL; sP = sP->next)
    {
      if (sP->type != CorString) continue;
      for (int i = 0; i < N; i++)
        if (strcmp(idV[i], sP->value.s) == 0) { anyOkV[i] = true; break; }
    }
  }

  if (remoteErrors != NULL && remoteErrors->type == CorArray)
  {
    for (CorNode* eP = remoteErrors->value.head; eP != NULL; eP = eP->next)
    {
      CorNode* idP    = corTreeLookup(eP, "entityId");
      CorNode* errP   = corTreeLookup(eP, "error");
      const char* eid = (idP != NULL && idP->type == CorString) ? idP->value.s : "";

      const char* type   = LD_ERROR_INTERNAL_ERROR;
      const char* title  = "Bad Gateway";
      const char* detail = "forward error";
      int         status = 502;
      if (errP != NULL && errP->type == CorObject)
      {
        CorNode* tP = corTreeLookup(errP, "type");
        CorNode* hP = corTreeLookup(errP, "title");
        CorNode* dP = corTreeLookup(errP, "detail");
        CorNode* sP = corTreeLookup(errP, "status");
        if (tP != NULL && tP->type == CorString) type   = tP->value.s;
        if (hP != NULL && hP->type == CorString) title  = hP->value.s;
        if (dP != NULL && dP->type == CorString) detail = dP->value.s;
        if (sP != NULL && sP->type == CorInt)    status = sP->value.i;
      }
      addBatchError(errorsP, eid, status, type, title, detail, csrRegId);
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
  CorNode**    fragV;
  int          count;
  int          capacity;
} Group;



typedef struct CsrAccum
{
  LdRegCacheItem* csr;
  LdRegMode       mode;
  CorNode**       fragV;
  const char**    idV;
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
    Group* newV = (Group*) kaAlloc(&corRest.kalloc, newCap * sizeof(Group));
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



static void groupFragAppend(Group* g, CorNode* fragP)
{
  if (g->count >= g->capacity)
  {
    int newCap = (g->capacity == 0) ? 4 : g->capacity * 2;
    CorNode** newV = (CorNode**) kaAlloc(&corRest.kalloc, newCap * sizeof(CorNode*));
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



static void csrAccumAppend(CsrAccum* a, CorNode* fragP, const char* id)
{
  if (a->count >= a->capacity)
  {
    int newCap = (a->capacity == 0) ? 4 : a->capacity * 2;
    CorNode**    newF  = (CorNode**)    kaAlloc(&corRest.kalloc, newCap * sizeof(CorNode*));
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
                         CorNode*     fragP,
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
      CorNode* chopped = ldEntityFragmentForInfo(fragP, riP, corRest.kallocP, detach);
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
// call has run with detach=false. We have to defer the detach until all
// redirect CSRs covering this entity have been served, otherwise the
// second-and-onwards ones find an empty fragment (D008_01_red-class
// bug; surfaced here by ETSI D013_01/02_red).
//
static void purgeRedirAttrsFromFragment(Tenant*     tenantP,
                                         const char* entityId,
                                         char**      typeArr,
                                         char**      scopeV,
                                         CorNode*    fragP,
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
      (void) ldEntityFragmentForInfo(fragP, riP, corRest.kallocP, /*detach=*/true);
  }

  ldRegCacheMatchRelease(matchV, matchN);
}



static bool hasAnyNonKeywordAttr(CorNode* fragP)
{
  if (fragP == NULL || fragP->type != CorObject) return false;
  for (CorNode* c = fragP->value.head; c != NULL; c = c->next)
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
// postEntityBatchUpsert -
//
bool postEntityBatchUpsert(void)
{
  CorNode* bodyP = corRest.in.requestTree;

  if (bodyP->type != CorArray)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Not a JSON Array",
            "Batch Entity Upsert body must be a JSON array");
    return true;
  }

  int total = 0;
  for (CorNode* c = bodyP->value.head; c != NULL; c = c->next)
  {
    if (c->type == CorNull)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Array Entry",
              "Batch Entity Upsert: null entry at position %d", total);
      return true;
    }
    total++;
  }

  bool hasPreErrors = (corNgsild.batchPreErrors != NULL &&
                       corNgsild.batchPreErrors->value.head != NULL);
  if (total == 0 && !hasPreErrors)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Empty Array",
            "Batch Entity Upsert: input array is empty");
    return true;
  }

  CorNode* successP = corTreeArray(corRest.kallocP, "success");
  CorNode* errorsP = corTreeArray(corRest.kallocP, "errors");

  if (hasPreErrors)
  {
    errorsP->value.head = corNgsild.batchPreErrors->value.head;
    errorsP->value.tail        = corNgsild.batchPreErrors->value.tail;
    corNgsild.batchPreErrors    = NULL;
  }

  bool updateMode = corNgsild.upsertUpdate;  // false (default) → replace semantics

  //
  // Pass 1 — validate + group by id.
  //
  Group* groups = NULL;
  int    gN     = 0;
  int    gCap   = 0;

  for (CorNode* inP = bodyP->value.head; inP != NULL; inP = inP->next)
  {
    if (inP->type != CorObject)
    {
      addBatchError(errorsP, "", 400,
                    LD_ERROR_BAD_REQUEST_DATA, "Invalid Array Entry",
                    "entity must be a JSON object", NULL);
      continue;
    }

    //
    // Normalization can REFUSE now - an attribute whose "type" names no NGSI-LD
    // Attribute type is an error, not something to infer a type for. It has
    // already set the problem, so it joins the existing check: short-circuit
    // means ldCheckEntity is skipped and the handling below reports it.
    //
    if (ldNormalizeInput(inP, &corRest.kalloc, false, false) == false ||
        ldCheckEntity(inP, LdOpCreateEntity, NULL, &corRest.kalloc) == false)
    {
      const char* eid = "";
      CorNode* idP = corTreeLookup(inP, "id");
      if (idP != NULL && idP->type == CorString) eid = idP->value.s;

      //
      // problemDetail is handed straight to addBatchError - no local copy. It
      // ends up in corTreeString(), which memcpy's the value into the CorNode's own
      // allocation, so the buffer is free to be reused on the next iteration
      // before this one is rendered.
      //
      // "Invalid Entity" is DELIBERATE, and decided here rather than taken from
      // corRest.out.problemTitle: an invalid attribute makes the whole entity
      // invalid, and the whole entity is what gets discarded. The specific title
      // the check function set is not lost - ldError logs it, with its own file
      // and line.
      //
      addBatchError(errorsP, eid, 400,
                    LD_ERROR_BAD_REQUEST_DATA, "Invalid Entity", corRest.out.problemDetail, NULL);

      corRest.out.httpStatusCode   = 0;
      corRest.out.problemType      = NULL;
      corRest.out.problemTitle     = NULL;
      corRest.out.problemDetail[0] = 0;
      continue;
    }

    CorNode* idP = corTreeLookup(inP, "id");
    if (idP == NULL || idP->type != CorString)
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

    Group* g = groupFindOrCreate(&groups, &gN, &gCap, idP->value.s);
    groupFragAppend(g, inP);
  }

  if (gN == 0)
  {
    int singleStatus = ldBatchErrorsSingleStatus(errorsP);
    if (singleStatus > 0)
    {
      corRest.out.responseTree   = ldBatchErrorAsProblemDetails(errorsP);
      corRest.out.httpStatusCode = singleStatus;
    }
    else
    {
      CorNode* respBodyP = corTreeObject(corRest.kallocP, NULL);
      corTreeChildAdd(respBodyP, successP);
      corTreeChildAdd(respBodyP, errorsP);
      corRest.out.responseTree   = respBodyP;
      corRest.out.httpStatusCode = 207;
      corNgsild.rawResponse      = true;
    }
    return true;
  }

  //
  // Pass 2 — per group: retrieve (to decide create-vs-update branch),
  // chop, merge-in-order, defer notifications.
  //
  Tenant*      tenantP   = (Tenant*) corNgsild.tenantP;
  LdSubCache*  subCacheP = (LdSubCache*) tenantP->subCacheP;

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);

  bool dispatch = (corNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;

  CsrAccum*    csrAccums   = NULL;
  int          csrAccumsN  = 0;
  int          csrAccumsCap= 0;

  CorNode*     finalsCreate  = corTreeArray(corRest.kallocP, NULL); // new entities → bulk create
  CorNode*     finalsUpdate  = corTreeArray(corRest.kallocP, NULL); // existing entities → bulk replace
  const char** createIdV     = (const char**) kaAlloc(&corRest.kalloc, sizeof(char*) * gN);
  const char** updateIdV     = (const char**) kaAlloc(&corRest.kalloc, sizeof(char*) * gN);
  CorNode**    createEntityV = (CorNode**)    kaAlloc(&corRest.kalloc, sizeof(CorNode*) * gN);
  CorNode**    updateEntityV = (CorNode**)    kaAlloc(&corRest.kalloc, sizeof(CorNode*) * gN);

  //
  // Requests to the DDS side go FIRST, per fragment, before the bulk writes -
  // and what they sent is released once those are done. Only when a bridge
  // carries anything.
  //
  bool             requestsFirst = (channelOutCount() > 0);
  BridgeSyncDone** doneV    = NULL;
  int              doneN    = 0;

  if (requestsFirst == true)                              // nothing at all without a bridge - not even the count
  {
    int fragTotal = 0;

    for (int gi = 0; gi < gN; gi++)
      fragTotal += groups[gi].count;

    doneV = (BridgeSyncDone**) kaAlloc(&corRest.kalloc, sizeof(BridgeSyncDone*) * (fragTotal + 1));
  }
  int          createN       = 0;
  int          updateN       = 0;
  bool*        anySuccessV   = (bool*)        kaAlloc(&corRest.kalloc, sizeof(bool)  * gN);
  bool*        wasCreatedV   = (bool*)        kaAlloc(&corRest.kalloc, sizeof(bool)  * gN);
  const char** allIdV        = (const char**) kaAlloc(&corRest.kalloc, sizeof(char*) * gN);

  //
  // ⭐ TWO LOOPS, split where the DDS step waits. Loop 1 chops each fragment and
  // SENDS its requests to the DDS side; every goal of the batch is then waited
  // for against one deadline; loop 2 applies what the transport accepted. In
  // one loop, each Entity's goals would wait before the next Entity's were even
  // sent - a batch's wait the sum of its goals', not the slowest of them.
  //
  // What crosses from one loop to the other is per Entity (a group) or per
  // fragment, below. Errors are collected per Entity and joined in batch order
  // at the end, so the split does not reorder errors[].
  //
  int fragAll = 0;

  for (int gi = 0; gi < gN; gi++)
    fragAll += groups[gi].count;

  CorNode**        existingDbV  = (CorNode**) kaAlloc(&corRest.kalloc, sizeof(CorNode*) * gN);
  CorNode**        groupErrorsV = (CorNode**) kaAlloc(&corRest.kalloc, sizeof(CorNode*) * gN);
  bool*            groupLiveV   = (bool*)    kaAlloc(&corRest.kalloc, sizeof(bool)    * gN);   // got past the retrieve
  bool*            existsV      = (bool*)    kaAlloc(&corRest.kalloc, sizeof(bool)    * gN);
  int*             fragBaseV    = (int*)     kaAlloc(&corRest.kalloc, sizeof(int)     * gN);   // a group's first fragment, in the per-fragment arrays
  bool*            readyV       = (bool*)    kaAlloc(&corRest.kalloc, sizeof(bool)    * (fragAll + 1));   // made it through loop 1
  BridgeSyncDone** fragDoneV    = (requestsFirst == true) ? (BridgeSyncDone**) kaAlloc(&corRest.kalloc, sizeof(BridgeSyncDone*) * (fragAll + 1)) : NULL;

  memset(groupErrorsV, 0, sizeof(CorNode*) * gN);
  memset(groupLiveV,   0, sizeof(bool)    * gN);
  memset(readyV,       0, sizeof(bool)    * (fragAll + 1));

  if (fragDoneV != NULL)
    memset(fragDoneV, 0, sizeof(BridgeSyncDone*) * (fragAll + 1));

  for (int gi = 0, base = 0; gi < gN; gi++)
  {
    fragBaseV[gi]  = base;
    base          += groups[gi].count;
  }

  //
  // Loop 1 - chop, and send.
  //
  for (int gi = 0; gi < gN; gi++)
  {
    Group*  g            = &groups[gi];
    CorNode* groupErrorsP = corTreeArray(corRest.kallocP, NULL);

    groupErrorsV[gi]   = groupErrorsP;
    allIdV[gi]         = g->id;
    anySuccessV[gi]    = false;
    wasCreatedV[gi]    = false;

    if (db.entityRetrieve == NULL)
    {
      addBatchError(groupErrorsP, g->id, 500,
                    LD_ERROR_INTERNAL_ERROR, "Internal Error",
                    "entityRetrieve not supported by this DB plugin", NULL);
      continue;
    }

    CorNode* existingDb = NULL;
    int r = db.entityRetrieve(tenantP, g->id, &existingDb);

    bool exists = (r == DB_OK && existingDb != NULL);
    if (!exists && r != DB_NOT_FOUND && r != DB_OK)
    {
      addBatchError(groupErrorsP, g->id, 500,
                    LD_ERROR_INTERNAL_ERROR, "Internal Error",
                    "database error during retrieve", NULL);
      continue;
    }

    groupLiveV[gi]  = true;
    existingDbV[gi] = existingDb;
    existsV[gi]     = exists;

    //
    // Each fragment in array order: distops chop, then - the fragment being
    // final now - its requests to the DDS side are SENT. Applied in loop 2.
    //
    for (int fi = 0; fi < g->count; fi++)
    {
      CorNode* fragP = g->fragV[fi];

      // Track whether the fragment had any user-supplied attribute
      // BEFORE the chop runs. If the user typed in attrs and all of
      // them were chopped (claimed by exclusive / redirect CSRs), the
      // local store has nothing to write and the entity must NOT be
      // created locally as a bare {id, type} stub. Distinguishing this
      // from a legit "user posted only id/type/scope" upsert is the
      // whole point of preChopHadAttrs.
      bool preChopHadAttrs = hasAnyNonKeywordAttr(fragP);

      //
      // Distops chop (order: exclusive → redirect → inclusive).
      //
      if (dispatch)
      {
        CorNode* typeP = corTreeLookup(fragP, "type");
        char*   typeArr[2] = { NULL, NULL };
        if (typeP != NULL && typeP->type == CorString)
          typeArr[0] = typeP->value.s;

        CorNode* scopeP      = corTreeLookup(fragP, "scope");
        char*   scopeBuf[2]  = { NULL, NULL };
        char**  scopeV       = NULL;
        if (scopeP != NULL && scopeP->type == CorString)
        {
          scopeBuf[0] = scopeP->value.s;
          scopeV      = scopeBuf;
        }

        chopForMode(tenantP, g->id, typeArr, scopeV, fragP,
                    LdRegModeExclusive, true,
                    &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
        // Redirect: clone (multiple redirect CSRs may cover the same
        // entity, all must receive a copy), then purge the redirect-
        // claimed attrs once, so the local apply only sees what's left.
        chopForMode(tenantP, g->id, typeArr, scopeV, fragP,
                    LdRegModeRedirect, false,
                    &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
        purgeRedirAttrsFromFragment(tenantP, g->id, typeArr, scopeV, fragP, ownAlias);
        chopForMode(tenantP, g->id, typeArr, scopeV, fragP,
                    LdRegModeInclusive, false,
                    &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
      }

      //
      // Nothing-left-to-apply local shortcut: if the fragment's attrs
      // were all chopped, skip local merge for THIS instance. The
      // create/update decision still depends on whether THE GROUP sees
      // any local write by the end, so we only decide post-loop.
      //
      // Exception: a first-fragment with no non-keyword attrs in the
      // ORIGINAL user input (preChopHadAttrs == false) is a legit
      // upsert payload (entity carrying only id / type / scope, e.g.
      // building-with-two-types.jsonld). Without this carve-out the
      // entity-level shape change (type singular → array, scope
      // changes) wouldn't reach the bulk-update path and the test
      // 004_03_05 / 004_08_01 type-mutation assertion fails. But if
      // the user DID supply attrs and the chop emptied the fragment,
      // skip local — the entity has nothing to live by here.
      //
      bool firstFragmentEmpty = (fi == 0) && !hasAnyNonKeywordAttr(fragP) && !preChopHadAttrs;
      if (!hasAnyNonKeywordAttr(fragP) && !firstFragmentEmpty)
        continue;

      ldApiEntityToDbModel(fragP, &corRest.kalloc, 0);

      readyV[fragBaseV[gi] + fi] = true;

      if (requestsFirst == true)
      {
        BridgeSyncDone* doneP = (BridgeSyncDone*) kaAlloc(&corRest.kalloc, sizeof(BridgeSyncDone));

        memset(doneP, 0, sizeof(BridgeSyncDone));
        doneV[doneN++]                = doneP;
        fragDoneV[fragBaseV[gi] + fi] = doneP;

        bridgeRequestsBeforeWrite(tenantP, g->id, fragP, BRIDGE_REQ_PER_ENTITY | BRIDGE_REQ_SEND_ONLY, doneP);
      }
    }
  }

  //
  // Every goal of the batch is out - now they are waited for, against ONE
  // deadline, so that a goal's answer never waits for the next goal to be
  // sent. A refused one is taken out of its fragment here.
  //
  if (requestsFirst == true)
  {
    int64_t dueMs = bridgeRequestsDeadline();

    for (int gi = 0; gi < gN; gi++)
    {
      for (int fi = 0; fi < groups[gi].count; fi++)
      {
        BridgeSyncDone* doneP = fragDoneV[fragBaseV[gi] + fi];

        if (doneP != NULL)
          bridgeRequestsAwait(groups[gi].fragV[fi], doneP, dueMs);
      }
    }
  }

  //
  // Loop 2 - each fragment the first loop made ready, in array order: what the
  // DDS step refused is reported, and the rest applied, notified and recorded.
  //
  for (int gi = 0; gi < gN; gi++)
  {
    if (groupLiveV[gi] == false)
      continue;

    Group*  g            = &groups[gi];
    CorNode* existingDb  = existingDbV[gi];
    bool    exists       = existsV[gi];
    CorNode* groupErrorsP = groupErrorsV[gi];

    // finalP is our in-memory running state. For upsert:
    //   - exists && replace mode: we'll set finalP to a clone of first frag
    //     AFTER the chop for the first frag (so chopped attrs aren't kept).
    //   - exists && update mode: finalP = existingDb; merge all frags in order.
    //   - !exists: finalP = clone of first frag (post-chop); merge remaining.
    CorNode* finalP = exists ? existingDb : NULL;

    bool    anyLocal = false;

    for (int fi = 0; fi < g->count; fi++)
    {
      if (readyV[fragBaseV[gi] + fi] == false)
        continue;

      CorNode*        fragP = g->fragV[fi];
      BridgeSyncDone* doneP = (fragDoneV != NULL) ? fragDoneV[fragBaseV[gi] + fi] : NULL;

      //
      // A request to the DDS side that could not be sent, or a goal that was
      // refused, was left out of the fragment and is this Entity's error. In
      // the default (replace) mode leaving it out would DELETE it, so it keeps
      // the value it had - not written means unchanged, as for a replace of
      // one Entity.
      //
      if (doneP != NULL)
      {
        for (int ix = 0; ix < doneP->failedN; ix++)
        {
          addBatchError(groupErrorsP, g->id, doneP->failedStatusV[ix],
                        doneP->failedTypeV[ix],
                        doneP->failedTitleV[ix],
                        doneP->failedReasonV[ix], NULL);

          if (updateMode == false)
          {
            CorNode* prevP = (finalP != NULL) ? finalP : existingDb;
            CorNode* keepP = (prevP != NULL) ? corTreeLookup(prevP, doneP->failedAttrV[ix]) : NULL;

            if (keepP != NULL)
              corTreeChildAdd(fragP, corTreeClone(corRest.kallocP, keepP));
          }
        }
      }

      //
      // Branch: first fragment vs subsequent fragment.
      //
      LdMergeReport report = { NULL };
      bool firstFragment = (fi == 0);
      LdNotifyOp  notifyOp = LdNotifyEntityUpdate;

      if (firstFragment && !exists)
      {
        // First fragment for a MISSING entity → create.
        finalP    = corTreeClone(corRest.kallocP, fragP);
        notifyOp  = LdNotifyEntityCreate;
        wasCreatedV[gi] = true;
      }
      else if (!updateMode)
      {
        // § 5.5.11.2 default (replace) — every fragment fully replaces
        // the previous state. Synthesise a merge report between previous
        // finalP (or existingDb on the first iteration) and the new fragP
        // so attr-level notification triggers still fire.
        //
        // § 4.8 — entity-level createdAt is set when the entity was first
        // entered into the system; carry it over from prevP so a replace
        // doesn't reset it. modifiedAt gets stamped fresh by the DB
        // layer on every write.
        CorNode* prevP = (finalP != NULL) ? finalP : existingDb;
        CorNode* newFinalP = corTreeClone(corRest.kallocP, fragP);

        if (prevP != NULL)
        {
          CorNode* prevCreatedP = corTreeLookup(prevP, "createdAt");
          if (prevCreatedP != NULL && prevCreatedP->type == CorInt)
          {
            // ldApiEntityToDbModel stamped fragP with a fresh createdAt;
            // overwrite that with the previous entity's value (§ 4.8).
            CorNode* nCreated = corTreeLookup(newFinalP, "createdAt");
            if (nCreated == NULL)
              corTreeChildAdd(newFinalP, corTreeClone(corRest.kallocP, prevCreatedP));
            else if (nCreated->type == CorInt)
              nCreated->value.i = prevCreatedP->value.i;
          }

          // § 5.6.2.4 — replacing an attribute instance must keep its
          // original createdAt. DB-shape attrs are objects keyed by
          // datasetId: { "@none": {createdAt,...}, "urn:x": {...} }.
          // Walk newFinalP's attrs and patch instance-level createdAt
          // from the matching prev instance (same attr + datasetId).
          for (CorNode* nAttr = newFinalP->value.head; nAttr != NULL; nAttr = nAttr->next)
          {
            if (nAttr->name == NULL || ldIsEntityKeyword(nAttr->name)) continue;
            if (nAttr->type != CorObject)                               continue;

            CorNode* pAttr = corTreeLookup(prevP, nAttr->name);
            if (pAttr == NULL || pAttr->type != CorObject)              continue;

            for (CorNode* nInst = nAttr->value.head; nInst != NULL; nInst = nInst->next)
            {
              if (nInst->type != CorObject) continue;
              CorNode* pInst = corTreeLookup(pAttr, nInst->name);
              if (pInst == NULL || pInst->type != CorObject) continue;

              CorNode* pInstCreated = corTreeLookup(pInst, "createdAt");
              if (pInstCreated == NULL || pInstCreated->type != CorInt) continue;

              CorNode* nInstCreated = corTreeLookup(nInst, "createdAt");
              if (nInstCreated == NULL)
                corTreeChildAdd(nInst, corTreeClone(corRest.kallocP, pInstCreated));
              else if (nInstCreated->type == CorInt)
                nInstCreated->value.i = pInstCreated->value.i;
            }
          }
        }

        //
        // The change report of a Replace - the one PUT /entities/{id} makes.
        // A copy of it here skipped none of the Entity's own members (its
        // createdAt and modifiedAt came out as changed Attributes, in the
        // notifications and as rows of the temporal history) and carried no
        // preValue, so an instance the replace removed was never a deletion.
        //
        if (prevP != NULL)
          ldEntityReplaceReport(prevP, newFinalP, &report);
        else
        {
          report.changes = corTreeArray(corRest.kallocP, NULL);
          for (CorNode* fAttr = fragP->value.head; fAttr != NULL; fAttr = fAttr->next)
          {
            if (fAttr->name == NULL || ldIsNotAttributeName(fAttr->name))  continue;
            CorNode* chg = corTreeObject(corRest.kallocP, NULL);
            corTreeChildAdd(chg, corTreeString(corRest.kallocP, "attr", (char*) fAttr->name));
            corTreeChildAdd(chg, corTreeString(corRest.kallocP, "reason", (char*) "attributeCreated"));
            corTreeChildAdd(report.changes, chg);
          }
        }

        finalP = newFinalP;
      }
      else
      {
        // Update mode (?options=update): merge into running state.
        if (finalP == NULL)
          finalP = corTreeClone(corRest.kallocP, fragP);
        else
          ldEntityAttrsSet(finalP, fragP, true,
                           corRest.requestStartTime, &report, corRest.kallocP);
      }

      anyLocal = true;

      if (subCacheP != NULL)
      {
        CorNode* snapshot = corTreeClone(corRest.kallocP, finalP);
        ldNotifyDefer(subCacheP, snapshot, notifyOp,
                      (notifyOp == LdNotifyEntityUpdate) ? &report : NULL);
      }

      // TRoE: optimistic per-fragment events. For created entities,
      // emit one entityCreated; the per-attr breakdown comes from the
      // corDB plugin walking entitySnapshot at dispatch time. For
      // update mode, emit per-attr events from the merge report.
      {
        CorNode* tn = corTreeLookup(finalP, "type");
        const char* etype = (tn != NULL && tn->type == CorString) ? tn->value.s : NULL;

        if (notifyOp == LdNotifyEntityCreate)
        {
          TroeEvent* tevP = (TroeEvent*) kaAlloc(&corRest.kalloc, sizeof(TroeEvent));
          memset(tevP, 0, sizeof(*tevP));
          tevP->op             = TroeOpEntityCreated;
          tevP->tenantP        = tenantP;
          tevP->entityId       = g->id;
          tevP->entityType     = etype;
          tevP->modifiedAtNs   = corRest.requestStartTime;
          tevP->entitySnapshot = finalP;
          troeDeferEntityEvent(tevP);
        }
        else
        {
          troeDeferAttrEventsFromMerge(tenantP, g->id, etype, finalP, &report,
                                       corRest.requestStartTime);
        }
      }
    }

    if (!anyLocal)
      continue;  // everything chopped away; distops will carry the ids

    //
    // Route to the right bulk write slot.
    //
    if (wasCreatedV[gi])
    {
      createEntityV[createN] = finalP;
      createIdV[createN++]   = g->id;
      corTreeChildAdd(finalsCreate, finalP);
    }
    else
    {
      updateEntityV[updateN] = finalP;
      updateIdV[updateN++]   = g->id;
      corTreeChildAdd(finalsUpdate, finalP);
    }
  }

  //
  // Each Entity's errors, in batch order - as one loop would have reported them.
  //
  for (int gi = 0; gi < gN; gi++)
  {
    CorNode* errP = (groupErrorsV[gi] != NULL) ? groupErrorsV[gi]->value.head : NULL;

    while (errP != NULL)
    {
      CorNode* nextP = errP->next;

      errP->next = NULL;                              // ⚠ corTreeChildAdd would take the rest of the list along
      corTreeChildAdd(errorsP, errP);
      errP = nextP;
    }
  }

  //
  // Pass 3 — synchronous distops forward, one POST /entityOperations/upsert
  // per accumulating CSR. The forward inherits the incoming options mode via
  // query string so the remote makes the same create/update decision.
  //
  const char* forwardQueryString = updateMode ? "?options=update" : NULL;
  int         fwdQsLen           = (forwardQueryString != NULL) ? (int) strlen(forwardQueryString) : 0;

  {
    LdDistOpBatchItem*   bItems   = (LdDistOpBatchItem*)   kaAlloc(&corRest.kalloc, csrAccumsN * sizeof(LdDistOpBatchItem));
    memset(bItems, 0, csrAccumsN * sizeof(LdDistOpBatchItem));
    LdDistOpBatchResult* bResults = (LdDistOpBatchResult*) kaAlloc(&corRest.kalloc, csrAccumsN * sizeof(LdDistOpBatchResult));
    int                  bIdx[csrAccumsN];
    int                  bCount   = 0;
    memset(bResults, 0, csrAccumsN * sizeof(LdDistOpBatchResult));

    const char* batchPath    = "/ngsi-ld/v1/entityOperations/upsert";
    int         batchPathLen = strlen(batchPath);

    for (int ai = 0; ai < csrAccumsN; ai++)
    {
      CsrAccum*       a   = &csrAccums[ai];
      LdRegCacheItem* csr = a->csr;

      if (a->count == 0) continue;

      if (!ldRegOpSupported(csr, LdOpBatchUpsert))
      {
        if (a->mode == LdRegModeExclusive || a->mode == LdRegModeRedirect)
        {
          const char* detail = (a->mode == LdRegModeExclusive)
                               ? "exclusive registration does not support upsertBatch"
                               : "redirect registration does not support upsertBatch";
          for (int i = 0; i < a->count; i++)
            addBatchError(errorsP, a->idV[i], 409,
                          LD_ERROR_CONFLICT, "Conflict", detail, csr->regId);
        }
        continue;
      }

      CorNode* batchArr = corTreeArray(corRest.kallocP, NULL);
      for (int i = 0; i < a->count; i++)
        corTreeChildAdd(batchArr, a->fragV[i]);

      int   baseLen = strlen(csr->endpoint);
      char* url     = (char*) kaAlloc(&corRest.kalloc, baseLen + batchPathLen + fwdQsLen + 1);
      strcpy(url, csr->endpoint);
      strcpy(url + baseLen, batchPath);
      if (fwdQsLen > 0) strcpy(url + baseLen + batchPathLen, forwardQueryString);

      char* body = renderBatchBody(csr, batchArr);

      COR_T(KtDistOpRequest, "forward: POST %s", url);
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

        CorNode* respTreeP = NULL;
        if (bResults[bi].responseBody != NULL && bResults[bi].responseBodyLen > 0)
        {
          CorNode* treeP = bResults[bi].responseTree;
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
    }
  }

  //
  // Pass 4 — bulk DB writes: create for new ids, update for existing.
  //
  if (createN > 0)
  {
    if (db.entityBulkCreate == NULL)
    {
      ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
              "Batch Entity Upsert (create path) not supported by this DB plugin");
      return true;
    }
    int* resultsV = (int*) kaAlloc(&corRest.kalloc, sizeof(int) * createN);
    db.entityBulkCreate(tenantP, finalsCreate, resultsV);

    //
    // Released after the LAST bulk write - the update path's below, when there
    // is one - so that nothing sent before either lands before it.
    //

    for (int k = 0; k < createN; k++)
    {
      const char* eid = createIdV[k];
      switch (resultsV[k])
      {
        case DB_OK:
          for (int gi = 0; gi < gN; gi++)
            if (strcmp(allIdV[gi], eid) == 0) { anySuccessV[gi] = true; break; }
          break;
        case DB_ALREADY_EXISTS:
          // Rare race: entity appeared between retrieve and bulk-create.
          addBatchError(errorsP, eid, 409,
                        LD_ERROR_ALREADY_EXISTS, "Already Exists",
                        "entity appeared between retrieve and bulk create", NULL);
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
                        "database error during batch upsert create", NULL);
          break;
      }
    }
  }

  if (updateN > 0)
  {
    if (db.entityBulkUpdate == NULL)
    {
      ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
              "Batch Entity Upsert (update path) not supported by this DB plugin");
      return true;
    }
    int* resultsV = (int*) kaAlloc(&corRest.kalloc, sizeof(int) * updateN);
    db.entityBulkUpdate(tenantP, finalsUpdate, resultsV);

    for (int k = 0; k < updateN; k++)
    {
      const char* eid = updateIdV[k];
      switch (resultsV[k])
      {
        case DB_OK:
          for (int gi = 0; gi < gN; gi++)
            if (strcmp(allIdV[gi], eid) == 0) { anySuccessV[gi] = true; break; }
          break;
        case DB_NOT_FOUND:
          // Rare race: entity disappeared between retrieve and bulk-update.
          addBatchError(errorsP, eid, 404,
                        LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                        "entity vanished between retrieve and bulk update", NULL);
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
                        "database error during batch upsert update", NULL);
          break;
      }
    }
  }

  //
  // Both bulk writes are done: what went to the DDS side before them may land.
  //
  for (int ix = 0; ix < doneN; ix++)
    bridgeRequestsWritten(doneV[ix]);

  //
  // Pass 5 — response.
  //
  int successCount = 0;
  int createdCount = 0;
  for (int gi = 0; gi < gN; gi++)
  {
    if (!anySuccessV[gi])
      continue;
    corTreeChildAdd(successP, corTreeString(corRest.kallocP, NULL, (char*) allIdV[gi]));
    successCount++;
    if (wasCreatedV[gi])
      createdCount++;
  }

  int errorCount = 0;
  for (CorNode* p = errorsP->value.head; p != NULL; p = p->next) errorCount++;

  //
  // Status code per § 6.15.3.1:
  //   201 Created — all entities succeeded AND at least one was created.
  //   204 No Content — all succeeded AND none were created (i.e. all updates).
  //   207 Multi-Status — some success + some errors.
  //   409 Conflict — all failed.
  //
  // § 5.6.8 — batch upsert: 201 if anything was created, 204 if all updates,
  // both also require errors=0. The (success=0, errors=0) case is success too.
  if (errorCount == 0)
  {
    corRest.out.httpStatusCode = (createdCount > 0) ? 201 : 204;
    if (createdCount > 0)
    {
      // § 5.6.8.5: 201 body is the array of newly-created entity IDs (the
      // "S Array"), not the BatchOperationResult shape — that's reserved
      // for 207 Multi-Status.
      CorNode* createdP = corTreeArray(corRest.kallocP, NULL);
      for (int gi = 0; gi < gN; gi++)
      {
        if (anySuccessV[gi] && wasCreatedV[gi])
          corTreeChildAdd(createdP, corTreeString(corRest.kallocP, NULL, (char*) allIdV[gi]));
      }
      corRest.out.responseTree = createdP;
      corNgsild.rawResponse    = true;
    }
    return true;
  }

  int singleStatus = (successCount == 0) ? ldBatchErrorsSingleStatus(errorsP) : -1;
  if (singleStatus > 0)
  {
    corRest.out.responseTree   = ldBatchErrorAsProblemDetails(errorsP);
    corRest.out.httpStatusCode = singleStatus;
  }
  else
  {
    CorNode* respBodyP = corTreeObject(corRest.kallocP, NULL);
    corTreeChildAdd(respBodyP, successP);
    corTreeChildAdd(respBodyP, errorsP);
    corRest.out.responseTree   = respBodyP;
    corRest.out.httpStatusCode = 207;
  }
  corNgsild.rawResponse      = true;
  return true;
}

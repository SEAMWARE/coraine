//
// FILE            postEntityBatchUpsert.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
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
#include "swNgsild/LdOp.h"                           // LdOpCreateEntity, LdOpUpdateAttrs, LdOpBatchUpsert
#include "swNgsild/LdNormalizeInput.h"               // ldNormalizeInput
#include "swNgsild/ldCheckEntity.h"                  // ldCheckEntity
#include "swNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "swNgsild/ldIsEntityKeyword.h"              // ldIsEntityKeyword
#include "swNgsild/ldStripAtContext.h"              // ldStripAtContext
#include "swNgsild/LdProblem.h"                      // LD_ERROR_*
#include "swNgsild/ldEntityAttrsSet.h"               // ldEntityAttrsSet
#include "swNgsild/ldEntityMerge.h"                  // LdMergeReport
#include "swNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityCreate, LdNotifyEntityUpdate
#include "swNgsild/ldNotifyDefer.h"                  // ldNotifyDefer

#include "troe/TroeDriver.h"                         // TroeEvent, TroeOpEntityCreated
#include "troe/troeDispatch.h"                       // troeDeferEntityEvent
#include "troe/troeFromMerge.h"                      // troeDeferAttrEventsFromMerge
#include "swNgsild/LdSubCache.h"                     // LdSubCache

#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "swNgsild/ldDistOp.h"                       // ldDistOp*
#include "swNgsild/ldEntityFragment.h"               // ldEntityFragmentForInfo

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND, DB_ERR, DB_ALREADY_EXISTS
#include "db/Tenant.h"                               // Tenant

#include "ktrace/kTrace.h"                           // KT_T
#include "swBrokerTraceLevels.h"                     // KtDistOpRequest

#include "serviceRoutines/postEntityBatchUpsert.h"   // Own interface



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
// csrJsonldContext - return jsonldContext URL for a CSR or NULL.
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
    // Strip body @context: forward goes out as application/json + Link.
    for (KjNode* fragP = batchArr->value.firstChildP; fragP != NULL; fragP = fragP->next)
    {
      KjNode* atCtx = kjLookup(fragP, "@context");
      if (atCtx != NULL)
        kjChildRemove(fragP, atCtx);
    }
  }

  int   bufSize = kjFastRenderSize(batchArr) + 1;
  char* buf     = (char*) kaAlloc(&swRest.kalloc, bufSize);
  kjFastRender(batchArr, buf);
  return buf;
}



static int forwardBatchToCSR(LdRegCacheItem* csr, KjNode* batchArr,
                              const char* ownAlias, KjNode** respTreePP,
                              const char* queryString /* or NULL */)
{
  *respTreePP = NULL;

  const char* path = "/ngsi-ld/v1/entityOperations/upsert";
  int         baseLen = strlen(csr->endpoint);
  int         pathLen = strlen(path);
  int         qsLen   = (queryString != NULL) ? strlen(queryString) : 0;
  char*       url     = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + qsLen + 1);
  strcpy(url, csr->endpoint);
  strcpy(url + baseLen, path);
  if (qsLen > 0) strcpy(url + baseLen + pathLen, queryString);

  char* body    = renderBatchBody(csr, batchArr);
  int   bodyLen = strlen(body);

  char*       respBody    = NULL;
  int         respBodyLen = 0;
  const char* upErr       = NULL;

  KT_T(KtDistOpRequest, "forward: POST %s", url);

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
// Group - fragments targeting one entity id, in arrival order.
//
typedef struct Group
{
  const char*  id;
  KjNode**     fragV;
  int          count;
  int          capacity;
} Group;



typedef struct CsrAccum
{
  LdRegCacheItem* csr;
  LdRegMode       mode;
  KjNode**        fragV;
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
// buildReplaceBase - produce the starting point for a "replace"-mode first
// fragment against an existing entity.
//
// The first fragment IS the entity's new state under replace semantics,
// so we don't merge into existing — we just take a clone of the fragment
// and call that the starting point. The id/type from the fragment are
// present; @context is dropped. Subsequent fragments will merge into it.
//
static KjNode* buildReplaceBase(KjNode* fragP)
{
  KjNode* baseP = kjClone(swRest.kjsonP, fragP);
  KjNode* atCtx = kjLookup(baseP, "@context");
  if (atCtx != NULL)
    kjChildRemove(baseP, atCtx);
  return baseP;
}



// -----------------------------------------------------------------------------
//
// postEntityBatchUpsert -
//
bool postEntityBatchUpsert(void)
{
  KjNode* bodyP = swRest.in.requestTree;

  if (bodyP->type != KjArray)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Batch Entity Upsert body must be a JSON array");
    return true;
  }

  int total = 0;
  for (KjNode* c = bodyP->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->type == KjNull)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "Batch Entity Upsert: null entry at position %d", total);
      return true;
    }
    total++;
  }

  bool hasPreErrors = (swNgsild.batchPreErrors != NULL &&
                       swNgsild.batchPreErrors->value.firstChildP != NULL);
  if (total == 0 && !hasPreErrors)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Batch Entity Upsert: input array is empty");
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

  bool updateMode = swNgsild.upsertUpdate;  // false (default) → replace semantics

  //
  // Pass 1 — validate + group by id.
  //
  Group* groups = NULL;
  int    gN     = 0;
  int    gCap   = 0;

  for (KjNode* inP = bodyP->value.firstChildP; inP != NULL; inP = inP->next)
  {
    if (inP->type != KjObject)
    {
      addBatchError(errorsP, "", 400,
                    LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
                    "entity must be a JSON object", NULL);
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
      continue;
    }

    KjNode* idP = kjLookup(inP, "id");
    if (idP == NULL || idP->type != KjString)
    {
      addBatchError(errorsP, "", 400,
                    LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
                    "entity id is missing or not a string", NULL);
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
  // Pass 2 — per group: retrieve (to decide create-vs-update branch),
  // chop, merge-in-order, defer notifications.
  //
  Tenant*      tenantP   = (Tenant*) swNgsild.tenantP;
  LdSubCache*  subCacheP = (LdSubCache*) tenantP->subCacheP;

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

  bool dispatch = (swNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;

  CsrAccum*    csrAccums   = NULL;
  int          csrAccumsN  = 0;
  int          csrAccumsCap= 0;

  KjNode*      finalsCreate  = kjArray(swRest.kjsonP, NULL);  // new entities → bulk create
  KjNode*      finalsUpdate  = kjArray(swRest.kjsonP, NULL);  // existing entities → bulk replace
  const char** createIdV     = (const char**) kaAlloc(&swRest.kalloc, sizeof(char*) * gN);
  const char** updateIdV     = (const char**) kaAlloc(&swRest.kalloc, sizeof(char*) * gN);
  int          createN       = 0;
  int          updateN       = 0;
  bool*        anySuccessV   = (bool*)        kaAlloc(&swRest.kalloc, sizeof(bool)  * gN);
  bool*        wasCreatedV   = (bool*)        kaAlloc(&swRest.kalloc, sizeof(bool)  * gN);
  const char** allIdV        = (const char**) kaAlloc(&swRest.kalloc, sizeof(char*) * gN);

  for (int gi = 0; gi < gN; gi++)
  {
    Group* g           = &groups[gi];
    allIdV[gi]         = g->id;
    anySuccessV[gi]    = false;
    wasCreatedV[gi]    = false;

    if (db.entityRetrieve == NULL)
    {
      addBatchError(errorsP, g->id, 500,
                    LD_ERROR_INTERNAL_ERROR, "Internal Error",
                    "entityRetrieve not supported by this DB plugin", NULL);
      continue;
    }

    KjNode* existingDb = NULL;
    int r = db.entityRetrieve(tenantP, g->id, &existingDb);

    bool exists = (r == DB_OK && existingDb != NULL);
    if (!exists && r != DB_NOT_FOUND && r != DB_OK)
    {
      addBatchError(errorsP, g->id, 500,
                    LD_ERROR_INTERNAL_ERROR, "Internal Error",
                    "database error during retrieve", NULL);
      continue;
    }

    // finalP is our in-memory running state. For upsert:
    //   - exists && replace mode: we'll set finalP to a clone of first frag
    //     AFTER the chop for the first frag (so chopped attrs aren't kept).
    //   - exists && update mode: finalP = existingDb; merge all frags in order.
    //   - !exists: finalP = clone of first frag (post-chop); merge remaining.
    KjNode* finalP = exists ? existingDb : NULL;

    bool    anyLocal = false;

    for (int fi = 0; fi < g->count; fi++)
    {
      KjNode* fragP = g->fragV[fi];

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

      ldApiEntityToDbModel(fragP, &swRest.kalloc);

      //
      // Branch: first fragment vs subsequent fragment.
      //
      LdMergeReport report = { NULL };
      bool firstFragment = (fi == 0);
      LdNotifyOp  notifyOp = LdNotifyEntityUpdate;

      if (firstFragment && !exists)
      {
        // First fragment for a MISSING entity → create.
        finalP    = kjClone(swRest.kjsonP, fragP);
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
        KjNode* prevP = (finalP != NULL) ? finalP : existingDb;
        KjNode* newFinalP = kjClone(swRest.kjsonP, fragP);

        if (prevP != NULL)
        {
          KjNode* prevCreatedP = kjLookup(prevP, "createdAt");
          if (prevCreatedP != NULL && prevCreatedP->type == KjInt)
          {
            // ldApiEntityToDbModel stamped fragP with a fresh createdAt;
            // overwrite that with the previous entity's value (§ 4.8).
            KjNode* nCreated = kjLookup(newFinalP, "createdAt");
            if (nCreated == NULL)
              kjChildAdd(newFinalP, kjClone(swRest.kjsonP, prevCreatedP));
            else if (nCreated->type == KjInt)
              nCreated->value.i = prevCreatedP->value.i;
          }

          // § 5.6.2.4 — replacing an attribute instance must keep its
          // original createdAt. DB-shape attrs are objects keyed by
          // datasetId: { "@none": {createdAt,...}, "urn:x": {...} }.
          // Walk newFinalP's attrs and patch instance-level createdAt
          // from the matching prev instance (same attr + datasetId).
          for (KjNode* nAttr = newFinalP->value.firstChildP; nAttr != NULL; nAttr = nAttr->next)
          {
            if (nAttr->name == NULL || ldIsEntityKeyword(nAttr->name)) continue;
            if (nAttr->type != KjObject)                                continue;

            KjNode* pAttr = kjLookup(prevP, nAttr->name);
            if (pAttr == NULL || pAttr->type != KjObject)               continue;

            for (KjNode* nInst = nAttr->value.firstChildP; nInst != NULL; nInst = nInst->next)
            {
              if (nInst->type != KjObject) continue;
              KjNode* pInst = kjLookup(pAttr, nInst->name);
              if (pInst == NULL || pInst->type != KjObject) continue;

              KjNode* pInstCreated = kjLookup(pInst, "createdAt");
              if (pInstCreated == NULL || pInstCreated->type != KjInt) continue;

              KjNode* nInstCreated = kjLookup(nInst, "createdAt");
              if (nInstCreated == NULL)
                kjChildAdd(nInst, kjClone(swRest.kjsonP, pInstCreated));
              else if (nInstCreated->type == KjInt)
                nInstCreated->value.i = pInstCreated->value.i;
            }
          }
        }

        report.changes = kjArray(swRest.kjsonP, NULL);
        if (prevP != NULL)
        {
          for (KjNode* eAttr = prevP->value.firstChildP; eAttr != NULL; eAttr = eAttr->next)
          {
            if (eAttr->name == NULL || eAttr->name[0] == '@')   continue;
            if (strcmp(eAttr->name, "id")   == 0)               continue;
            if (strcmp(eAttr->name, "type") == 0)               continue;
            if (kjLookup(fragP, eAttr->name) != NULL)           continue;
            KjNode* chg = kjObject(swRest.kjsonP, NULL);
            kjChildAdd(chg, kjString(swRest.kjsonP, "attr",   (char*) eAttr->name));
            kjChildAdd(chg, kjString(swRest.kjsonP, "reason", (char*) "attributeDeleted"));
            kjChildAdd(report.changes, chg);
          }
        }
        for (KjNode* fAttr = fragP->value.firstChildP; fAttr != NULL; fAttr = fAttr->next)
        {
          if (fAttr->name == NULL || fAttr->name[0] == '@')     continue;
          if (strcmp(fAttr->name, "id")   == 0)                  continue;
          if (strcmp(fAttr->name, "type") == 0)                  continue;
          const char* reason = (prevP != NULL && kjLookup(prevP, fAttr->name) != NULL)
                               ? "attributeModified"
                               : "attributeCreated";
          KjNode* chg = kjObject(swRest.kjsonP, NULL);
          kjChildAdd(chg, kjString(swRest.kjsonP, "attr",   (char*) fAttr->name));
          kjChildAdd(chg, kjString(swRest.kjsonP, "reason", (char*) reason));
          kjChildAdd(report.changes, chg);
        }

        finalP = newFinalP;
      }
      else
      {
        // Update mode (?options=update): merge into running state.
        if (finalP == NULL)
          finalP = kjClone(swRest.kjsonP, fragP);
        else
          ldEntityAttrsSet(finalP, fragP, true,
                           swRest.requestStartTime, &report, swRest.kjsonP);
      }

      anyLocal = true;

      if (subCacheP != NULL)
      {
        KjNode* snapshot = kjClone(swRest.kjsonP, finalP);
        ldNotifyDefer(subCacheP, snapshot, notifyOp,
                      (notifyOp == LdNotifyEntityUpdate) ? &report : NULL);
      }

      // TRoE: optimistic per-fragment events. For created entities,
      // emit one entityCreated; the per-attr breakdown comes from the
      // ramdb plugin walking entitySnapshot at dispatch time. For
      // update mode, emit per-attr events from the merge report.
      {
        KjNode* tn = kjLookup(finalP, "type");
        const char* etype = (tn != NULL && tn->type == KjString) ? tn->value.s : NULL;

        if (notifyOp == LdNotifyEntityCreate)
        {
          TroeEvent* tevP = (TroeEvent*) kaAlloc(&swRest.kalloc, sizeof(TroeEvent));
          memset(tevP, 0, sizeof(*tevP));
          tevP->op             = TroeOpEntityCreated;
          tevP->tenantP        = tenantP;
          tevP->entityId       = g->id;
          tevP->entityType     = etype;
          tevP->modifiedAtNs   = swRest.requestStartTime;
          tevP->entitySnapshot = finalP;
          troeDeferEntityEvent(tevP);
        }
        else
        {
          troeDeferAttrEventsFromMerge(tenantP, g->id, etype, finalP, &report,
                                       swRest.requestStartTime);
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
      createIdV[createN++] = g->id;
      kjChildAdd(finalsCreate, finalP);
    }
    else
    {
      updateIdV[updateN++] = g->id;
      kjChildAdd(finalsUpdate, finalP);
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
    LdDistOpBatchItem*   bItems   = (LdDistOpBatchItem*)   kaAlloc(&swRest.kalloc, csrAccumsN * sizeof(LdDistOpBatchItem));
    LdDistOpBatchResult* bResults = (LdDistOpBatchResult*) kaAlloc(&swRest.kalloc, csrAccumsN * sizeof(LdDistOpBatchResult));
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

      KjNode* batchArr = kjArray(swRest.kjsonP, NULL);
      for (int i = 0; i < a->count; i++)
        kjChildAdd(batchArr, a->fragV[i]);

      int   baseLen = strlen(csr->endpoint);
      char* url     = (char*) kaAlloc(&swRest.kalloc, baseLen + batchPathLen + fwdQsLen + 1);
      strcpy(url, csr->endpoint);
      strcpy(url + baseLen, batchPath);
      if (fwdQsLen > 0) strcpy(url + baseLen + batchPathLen, forwardQueryString);

      char* body = renderBatchBody(csr, batchArr);

      KT_T(KtDistOpRequest, "forward: POST %s", url);
      bItems[bCount].csr     = csr;
      bItems[bCount].url     = url;
      bItems[bCount].body    = body;
      bItems[bCount].bodyLen = strlen(body);
      bIdx[bCount]           = ai;
      bCount++;
    }

    if (bCount > 0)
    {
      ldDistOpSendMulti(bItems, bCount, SwVerbPost, ownAlias, bResults);

      for (int bi = 0; bi < bCount; bi++)
      {
        CsrAccum* a = &csrAccums[bIdx[bi]];

        KjNode* respTreeP = NULL;
        if (bResults[bi].responseBody != NULL && bResults[bi].responseBodyLen > 0)
        {
          KjNode* treeP = kjParse(swRest.kjsonP, bResults[bi].responseBody);
          if (treeP != NULL)
          {
            ldStripAtContext(treeP);
            respTreeP = treeP;
          }
        }

        bool* groupOk = (bool*) kaAlloc(&swRest.kalloc, sizeof(bool) * a->count);
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
    int* resultsV = (int*) kaAlloc(&swRest.kalloc, sizeof(int) * createN);
    db.entityBulkCreate(tenantP, finalsCreate, resultsV);

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
    int* resultsV = (int*) kaAlloc(&swRest.kalloc, sizeof(int) * updateN);
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
        default:
          addBatchError(errorsP, eid, 500,
                        LD_ERROR_INTERNAL_ERROR, "Internal Error",
                        "database error during batch upsert update", NULL);
          break;
      }
    }
  }

  //
  // Pass 5 — response.
  //
  int successCount = 0;
  int createdCount = 0;
  for (int gi = 0; gi < gN; gi++)
  {
    if (!anySuccessV[gi])
      continue;
    kjChildAdd(successP, kjString(swRest.kjsonP, NULL, (char*) allIdV[gi]));
    successCount++;
    if (wasCreatedV[gi])
      createdCount++;
  }

  int errorCount = 0;
  for (KjNode* p = errorsP->value.firstChildP; p != NULL; p = p->next) errorCount++;

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
    swRest.out.httpStatusCode = (createdCount > 0) ? 201 : 204;
    if (createdCount > 0)
    {
      // § 5.6.8.5: 201 body is the array of newly-created entity IDs (the
      // "S Array"), not the BatchOperationResult shape — that's reserved
      // for 207 Multi-Status.
      KjNode* createdP = kjArray(swRest.kjsonP, NULL);
      for (int gi = 0; gi < gN; gi++)
      {
        if (anySuccessV[gi] && wasCreatedV[gi])
          kjChildAdd(createdP, kjString(swRest.kjsonP, NULL, (char*) allIdV[gi]));
      }
      swRest.out.responseTree = createdP;
      swNgsild.rawResponse    = true;
    }
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
  }
  swNgsild.rawResponse      = true;
  return true;
}

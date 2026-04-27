//
// FILE            postEntityBatchDelete.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/entityOperations/delete — Batch Entity Delete (§ 5.6.11).
//
// Body: JSON array of URI strings (entity ids). No fragments.
//
// Flow:
//   Pass 1 — parse + validate. Reject non-strings / non-URIs as
//            per-entity errors; build parallel idV / flag arrays.
//   Pass 2 — CSR matching per id (with NULL type — batch delete body
//            has no type info). Accumulate each match per CSR for
//            forwarding.
//   Pass 3 — synchronous distops forward, one POST
//            /entityOperations/delete per CSR, body = JSON array of
//            its matched ids.
//   Pass 4 — db.entityBulkDelete for the local side, fetching
//            pre-delete snapshots for notifications.
//   Pass 5 — per successful id: defer one EntityDelete notification
//            with the pre-delete snapshot.
//   Pass 6 — response: BatchOperationResult (§ 5.2.17), 204 all-OK,
//            207 partial, 409 all-failed.
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp, strlen, strcpy, strncpy
#include <strings.h>                                 // strcasecmp
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

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdOp.h"                           // LdOpBatchDelete
#include "swNgsild/LdProblem.h"                      // LD_ERROR_*
#include "swNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityDelete
#include "swNgsild/ldStripAtContext.h"              // ldStripAtContext
#include "swNgsild/ldNotifyDefer.h"                  // ldNotifyDefer

#include "troe/TroeDriver.h"                         // TroeEvent, TroeOpEntityDeleted
#include "troe/troeDispatch.h"                       // troeDeferEntityEvent
#include "swNgsild/LdSubCache.h"                     // LdSubCache

#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "swNgsild/ldDistOp.h"                       // ldDistOp*

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postEntityBatchDelete.h"   // Own interface



// -----------------------------------------------------------------------------
//
// addBatchError -
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
// CsrAccum - list of ids accumulating per CSR for forwarding.
//
typedef struct CsrAccum
{
  LdRegCacheItem* csr;
  LdRegMode       mode;
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
  (*aV)[*aN].idV      = NULL;
  (*aV)[*aN].count    = 0;
  (*aV)[*aN].capacity = 0;
  return &(*aV)[(*aN)++];
}



static void csrAccumAppend(CsrAccum* a, const char* id)
{
  if (a->count >= a->capacity)
  {
    int newCap = (a->capacity == 0) ? 4 : a->capacity * 2;
    const char** newV = (const char**) kaAlloc(&swRest.kalloc, newCap * sizeof(char*));
    for (int i = 0; i < a->count; i++) newV[i] = a->idV[i];
    a->idV      = newV;
    a->capacity = newCap;
  }
  a->idV[a->count++] = id;
}



static void matchCsrForMode(Tenant* tenantP, const char* entityId, LdRegMode mode,
                            CsrAccum** aVp, int* aNp, int* aCapP, const char* ownAlias)
{
  if (tenantP->regCacheP == NULL)
    return;

  LdRegCacheItem** matchV = NULL;
  int matchN = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                 entityId, NULL /* typeArr */, NULL /* scopeV */,
                                                 mode, &matchV);

  for (int m = 0; m < matchN; m++)
  {
    LdRegCacheItem* csr = matchV[m];
    if (csr->endpoint == NULL)                 continue;
    if (ldDistOpCsrWouldLoop(csr, ownAlias))   continue;

    CsrAccum* a = csrAccumFindOrCreate(aVp, aNp, aCapP, csr, mode);
    csrAccumAppend(a, entityId);
  }

  if (matchV != NULL) free(matchV);
}



// -----------------------------------------------------------------------------
//
// forwardBatchToCSR - POST a JSON array of ids to the CSR's
// /entityOperations/delete endpoint.
//
static int forwardBatchToCSR(LdRegCacheItem* csr, const char** idV, int N,
                             const char* ownAlias, KjNode** respTreePP)
{
  *respTreePP = NULL;

  const char* path    = "/ngsi-ld/v1/entityOperations/delete";
  int         baseLen = strlen(csr->endpoint);
  int         pathLen = strlen(path);
  char*       url     = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + 1);
  strcpy(url, csr->endpoint);
  strcpy(url + baseLen, path);

  // Body = JSON array of id strings. Build via kjson so rendering
  // handles quoting / escaping consistently.
  KjNode* arr = kjArray(swRest.kjsonP, NULL);
  for (int i = 0; i < N; i++)
    kjChildAdd(arr, kjString(swRest.kjsonP, NULL, (char*) idV[i]));

  int   bufSize = kjFastRenderSize(arr) + 1;
  char* body    = (char*) kaAlloc(&swRest.kalloc, bufSize);
  kjFastRender(arr, body);
  int bodyLen = strlen(body);

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
// postEntityBatchDelete -
//
bool postEntityBatchDelete(void)
{
  KjNode* bodyP = swRest.in.requestTree;

  if (bodyP->type != KjArray)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Batch Entity Delete body must be a JSON array");
    return true;
  }

  int total = 0;
  for (KjNode* c = bodyP->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->type == KjNull)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "Batch Entity Delete: null entry at position %d", total);
      return true;
    }
    total++;
  }

  if (total == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Batch Entity Delete: input array is empty");
    return true;
  }

  KjNode* successP = kjArray(swRest.kjsonP, "success");
  KjNode* errorsP  = kjArray(swRest.kjsonP, "errors");

  //
  // Pass 1 — validate each entry is a URI string, collect ids.
  //
  const char** idV = (const char**) kaAlloc(&swRest.kalloc, sizeof(char*) * total);
  int          n   = 0;

  for (KjNode* inP = bodyP->value.firstChildP; inP != NULL; inP = inP->next)
  {
    if (inP->type != KjString)
    {
      addBatchError(errorsP, "",
                    LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
                    "entry must be a URI string", NULL);
      continue;
    }
    if (inP->value.s == NULL || inP->value.s[0] == 0)
    {
      addBatchError(errorsP, "",
                    LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
                    "empty entity id", NULL);
      continue;
    }
    idV[n++] = inP->value.s;
  }

  if (n == 0)
  {
    KjNode* respBodyP = kjObject(swRest.kjsonP, NULL);
    kjChildAdd(respBodyP, successP);
    kjChildAdd(respBodyP, errorsP);
    swRest.out.responseTree   = respBodyP;
    swRest.out.httpStatusCode = 400;
    swNgsild.rawResponse      = true;
    return true;
  }

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;
  LdSubCache* subCacheP = (LdSubCache*) tenantP->subCacheP;

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

  bool dispatch = (swNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;

  // anySuccessV: per-id overall-success flag (local OR any distop).
  bool* anySuccessV = (bool*) kaAlloc(&swRest.kalloc, sizeof(bool) * n);
  for (int i = 0; i < n; i++) anySuccessV[i] = false;

  //
  // Pass 2 — CSR match per id. typeArr/scopeV NULL because batch delete
  // body carries no type info.
  //
  CsrAccum* csrAccums    = NULL;
  int       csrAccumsN   = 0;
  int       csrAccumsCap = 0;

  if (dispatch)
  {
    for (int i = 0; i < n; i++)
    {
      matchCsrForMode(tenantP, idV[i], LdRegModeExclusive,
                      &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
      matchCsrForMode(tenantP, idV[i], LdRegModeRedirect,
                      &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
      matchCsrForMode(tenantP, idV[i], LdRegModeInclusive,
                      &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
    }
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

    if (!ldRegOpSupported(csr, LdOpBatchDelete))
    {
      if (a->mode == LdRegModeExclusive || a->mode == LdRegModeRedirect)
      {
        const char* detail = (a->mode == LdRegModeExclusive)
                             ? "exclusive registration does not support deleteBatch"
                             : "redirect registration does not support deleteBatch";
        for (int i = 0; i < a->count; i++)
          addBatchError(errorsP, a->idV[i],
                        LD_ERROR_CONFLICT, "Conflict", detail, csr->regId);
      }
      continue;
    }

    KjNode* respTreeP = NULL;
    int     status    = forwardBatchToCSR(csr, a->idV, a->count, ownAlias, &respTreeP);

    bool* groupOk = (bool*) kaAlloc(&swRest.kalloc, sizeof(bool) * a->count);
    for (int k = 0; k < a->count; k++) groupOk[k] = false;

    applyRemoteBatchResult(status, respTreeP, csr->regId, errorsP,
                           groupOk, a->idV, a->count);

    for (int k = 0; k < a->count; k++)
    {
      if (!groupOk[k]) continue;
      for (int j = 0; j < n; j++)
      {
        if (strcmp(idV[j], a->idV[k]) == 0)
        {
          anySuccessV[j] = true;
          break;
        }
      }
    }
  }

  //
  // Pass 4 — bulk DB delete.
  //
  if (db.entityBulkDelete == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented",
            "Batch Entity Delete not supported by this DB plugin");
    return true;
  }

  int*     resultsV   = (int*)     kaAlloc(&swRest.kalloc, sizeof(int)     * n);
  KjNode** snapshotsV = (KjNode**) kaAlloc(&swRest.kalloc, sizeof(KjNode*) * n);
  for (int i = 0; i < n; i++)
  {
    resultsV[i]   = DB_NOT_FOUND;
    snapshotsV[i] = NULL;
  }

  db.entityBulkDelete(tenantP, idV, n, resultsV, snapshotsV);

  //
  // Pass 5 — per-id success + notification + error accumulation.
  //
  for (int i = 0; i < n; i++)
  {
    const char* eid = idV[i];

    switch (resultsV[i])
    {
      case DB_OK:
      {
        anySuccessV[i] = true;

        if (subCacheP != NULL && snapshotsV[i] != NULL)
          ldNotifyDeferDelete(subCacheP, snapshotsV[i], swRest.requestStartTime);

        // TRoE: defer one entity-level deleted tombstone per successful id.
        {
          const char* etype = NULL;
          if (snapshotsV[i] != NULL)
          {
            KjNode* tn = kjLookup(snapshotsV[i], "type");
            if (tn != NULL && tn->type == KjString) etype = tn->value.s;
          }
          TroeEvent* tevP = (TroeEvent*) kaAlloc(&swRest.kalloc, sizeof(TroeEvent));
          memset(tevP, 0, sizeof(*tevP));
          tevP->op             = TroeOpEntityDeleted;
          tevP->tenantP        = tenantP;
          tevP->entityId       = eid;
          tevP->entityType     = etype;
          tevP->modifiedAtNs   = swRest.requestStartTime;
          tevP->entitySnapshot = snapshotsV[i];
          troeDeferEntityEvent(tevP);
        }
        break;
      }
      case DB_NOT_FOUND:
        // If a distop already succeeded for this id, don't surface a local 404.
        if (!anySuccessV[i])
          addBatchError(errorsP, eid,
                        LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                        "entity does not exist", NULL);
        break;
      default:
        addBatchError(errorsP, eid,
                      LD_ERROR_INTERNAL_ERROR, "Internal Error",
                      "database error during batch delete", NULL);
        break;
    }
  }

  //
  // Pass 6 — response.
  //
  int successCount = 0;
  for (int i = 0; i < n; i++)
  {
    if (!anySuccessV[i]) continue;
    kjChildAdd(successP, kjString(swRest.kjsonP, NULL, (char*) idV[i]));
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

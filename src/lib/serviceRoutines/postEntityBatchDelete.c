//
// FILE            postEntityBatchDelete.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
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

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/CorRestVerb.h"                       // CorVerbPost

#include "corAlloc/corAlloc.h"                       // corAlloc
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeBuilder.h"                  // corTreeObject, corTreeArray, corTreeString, corTreeChildAdd
#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corJson/corJsonParse.h"                    // corJsonParse
#include "corJson/corJsonRender.h"                   // corJsonFastRender
#include "corJson/corJsonRenderSize.h"               // corJsonFastRenderSize

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdOp.h"                           // LdOpBatchDelete
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*
#include "corNgsild/ldCheckUri.h"                      // ldCheckUri
#include "corNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityDelete
#include "corNgsild/ldStripAtContext.h"              // ldStripAtContext
#include "corNgsild/ldNotifyDefer.h"                  // ldNotifyDefer

#include "troe/TroeDriver.h"                         // TroeEvent, TroeOpEntityDeleted
#include "troe/troeDispatch.h"                       // troeDeferEntityEvent
#include "corNgsild/LdSubCache.h"                     // LdSubCache

#include "corNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode
#include "corNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "corNgsild/ldDistOp.h"                       // ldDistOp*

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postEntityBatchDelete.h"   // Own interface



// -----------------------------------------------------------------------------
//
// addBatchError -
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
    CsrAccum* newV = (CsrAccum*) corAlloc(&corRest.kalloc, newCap * sizeof(CsrAccum));
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
    const char** newV = (const char**) corAlloc(&corRest.kalloc, newCap * sizeof(char*));
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

  ldRegCacheMatchRelease(matchV, matchN);
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
// postEntityBatchDelete -
//
bool postEntityBatchDelete(void)
{
  CorNode* bodyP = corRest.in.requestTree;

  if (bodyP->type != CorArray)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Not a JSON Array",
            "Batch Entity Delete body must be a JSON array");
    return true;
  }

  int total = 0;
  for (CorNode* c = bodyP->value.head; c != NULL; c = c->next)
  {
    if (c->type == CorNull)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Array Entry",
              "Batch Entity Delete: null entry at position %d", total);
      return true;
    }
    total++;
  }

  if (total == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Empty Array",
            "Batch Entity Delete: input array is empty");
    return true;
  }

  CorNode* successP = corTreeArray(corRest.kallocP, "success");
  CorNode* errorsP = corTreeArray(corRest.kallocP, "errors");

  //
  // Pass 1 — validate each entry is a URI string, collect ids.
  //
  const char** idV = (const char**) corAlloc(&corRest.kalloc, sizeof(char*) * total);
  int          n   = 0;

  for (CorNode* inP = bodyP->value.head; inP != NULL; inP = inP->next)
  {
    if (inP->type != CorString)
    {
      addBatchError(errorsP, "", 400,
                    LD_ERROR_BAD_REQUEST_DATA, "Invalid Array Entry",
                    "entry must be a URI string", NULL);
      continue;
    }
    if (inP->value.s == NULL || inP->value.s[0] == 0)
    {
      addBatchError(errorsP, "", 400,
                    LD_ERROR_BAD_REQUEST_DATA, "Invalid Array Entry",
                    "empty entity id", NULL);
      continue;
    }
    // The input array is a list of Entity IDs (URIs) (§ 10.3.6.3) — a non-URI
    // item is malformed input, a per-entity 400, not a 404 (a missing entity).
    // Aligns with Batch Update/Merge, which reject an invalid entity id 400.
    if (ldCheckUri(inP->value.s) == false)
    {
      addBatchError(errorsP, inP->value.s, 400,
                    LD_ERROR_BAD_REQUEST_DATA, "Invalid Array Entry",
                    "entry is not a valid URI", NULL);
      continue;
    }
    idV[n++] = inP->value.s;
  }

  if (n == 0)
  {
    CorNode* respBodyP = corTreeObject(corRest.kallocP, NULL);
    corTreeChildAdd(respBodyP, successP);
    corTreeChildAdd(respBodyP, errorsP);
    corRest.out.responseTree   = respBodyP;
    corRest.out.httpStatusCode = 400;
    corNgsild.rawResponse      = true;
    return true;
  }

  Tenant* tenantP = (Tenant*) corNgsild.tenantP;
  LdSubCache* subCacheP = (LdSubCache*) tenantP->subCacheP;

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);

  bool dispatch = (corNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;

  // anySuccessV: per-id overall-success flag (local OR any distop).
  bool* anySuccessV = (bool*) corAlloc(&corRest.kalloc, sizeof(bool) * n);
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
  // Pass 3 — concurrent distops forward per CSR via ldDistOpSendMulti.
  //
  {
    LdDistOpBatchItem*   bItems   = (LdDistOpBatchItem*)   corAlloc(&corRest.kalloc, csrAccumsN * sizeof(LdDistOpBatchItem));
    memset(bItems, 0, csrAccumsN * sizeof(LdDistOpBatchItem));
    LdDistOpBatchResult* bResults = (LdDistOpBatchResult*) corAlloc(&corRest.kalloc, csrAccumsN * sizeof(LdDistOpBatchResult));
    int                  bIdx[csrAccumsN];
    int                  bCount   = 0;
    memset(bResults, 0, csrAccumsN * sizeof(LdDistOpBatchResult));

    const char* batchPath = "/ngsi-ld/v1/entityOperations/delete";
    int         batchPathLen = strlen(batchPath);

    for (int ai = 0; ai < csrAccumsN; ai++)
    {
      CsrAccum*       a   = &csrAccums[ai];
      LdRegCacheItem* csr = a->csr;

      if (a->count == 0) continue;

      if (!ldRegOpSupported(csr, LdOpBatchDelete))
      {
        if (a->mode == LdRegModeExclusive || a->mode == LdRegModeRedirect)
        {
          const char* detail = (a->mode == LdRegModeExclusive)
                               ? "exclusive registration does not support deleteBatch"
                               : "redirect registration does not support deleteBatch";
          for (int i = 0; i < a->count; i++)
            addBatchError(errorsP, a->idV[i], 409,
                          LD_ERROR_CONFLICT, "Conflict", detail, csr->regId);
        }
        continue;
      }

      int   baseLen = strlen(csr->endpoint);
      char* url     = (char*) corAlloc(&corRest.kalloc, baseLen + batchPathLen + 1);
      strcpy(url, csr->endpoint);
      strcpy(url + baseLen, batchPath);

      CorNode* arr = corTreeArray(corRest.kallocP, NULL);
      for (int i = 0; i < a->count; i++)
        corTreeChildAdd(arr, corTreeString(corRest.kallocP, NULL, (char*) a->idV[i]));
      int   bufSize = corJsonFastRenderSize(arr) + 1;
      char* body    = (char*) corAlloc(&corRest.kalloc, bufSize);
      corJsonFastRender(arr, body);

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

        bool* groupOk = (bool*) corAlloc(&corRest.kalloc, sizeof(bool) * a->count);
        for (int k = 0; k < a->count; k++) groupOk[k] = false;

        applyRemoteBatchResult(bResults[bi].statusCode, respTreeP, bItems[bi].csr->regId,
                               errorsP, groupOk, a->idV, a->count);

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
    }
  }

  //
  // Pass 4 — bulk DB delete.
  //
  if (db.entityBulkDelete == NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
            "Batch Entity Delete not supported by this DB plugin");
    return true;
  }

  int*     resultsV   = (int*)     corAlloc(&corRest.kalloc, sizeof(int)    * n);
  CorNode** snapshotsV = (CorNode**) corAlloc(&corRest.kalloc, sizeof(CorNode*) * n);
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
          ldNotifyDeferDelete(subCacheP, snapshotsV[i], corRest.requestStartTime);

        // TRoE: defer one entity-level deleted tombstone per successful id.
        {
          const char* etype = NULL;
          if (snapshotsV[i] != NULL)
          {
            CorNode* tn = corTreeLookup(snapshotsV[i], "type");
            if (tn != NULL && tn->type == CorString) etype = tn->value.s;
          }
          TroeEvent* tevP = (TroeEvent*) corAlloc(&corRest.kalloc, sizeof(TroeEvent));
          memset(tevP, 0, sizeof(*tevP));
          tevP->op             = TroeOpEntityDeleted;
          tevP->tenantP        = tenantP;
          tevP->entityId       = eid;
          tevP->entityType     = etype;
          tevP->modifiedAtNs   = corRest.requestStartTime;
          tevP->entitySnapshot = snapshotsV[i];
          troeDeferEntityEvent(tevP);
        }
        break;
      }
      case DB_NOT_FOUND:
        // If a distop already succeeded for this id, don't surface a local 404.
        if (!anySuccessV[i])
          addBatchError(errorsP, eid, 404,
                        LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                        "entity does not exist", NULL);
        break;
      default:
        addBatchError(errorsP, eid, 500,
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
    corTreeChildAdd(successP, corTreeString(corRest.kallocP, NULL, (char*) idV[i]));
    successCount++;
  }

  int errorCount = 0;
  for (CorNode* p = errorsP->value.head; p != NULL; p = p->next) errorCount++;

  if (errorCount == 0)
  {
    corRest.out.httpStatusCode = 204;
    return true;
  }

  // § 5.6.10 / § 6.13.x: full failure with one common error status
  // collapses to that status + bare ProblemDetails (so deleting only
  // missing entities surfaces 404 with the proper body, not a 409
  // wrapper). Mixed / multi-status results stay 207 + {success, errors}.
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
    corNgsild.rawResponse      = true;
  }
  return true;
}

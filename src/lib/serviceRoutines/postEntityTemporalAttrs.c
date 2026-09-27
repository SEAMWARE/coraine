//
// FILE            postEntityTemporalAttrs.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// POST /ngsi-ld/v1/temporal/entities/{id}/attrs — § 5.6.12 / § 6.20.3.1.
// Add Attributes to Temporal Evolution. Body is an EntityTemporal Fragment;
// each per-attribute array of instances is appended to TRoE.
//
// Distops (§ 4.3.6 / § 5.6.12.4): exclusive/redirect chop matching attrs
// out of the local body before the local append; inclusive forwards a
// clone and keeps everything locally. CSRs must support
// appendAttrsTemporal (NOT in any default group per § 4.20 Table 4.20-2).
//

#include <stddef.h>                                  // NULL
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // free
#include <string.h>                                  // strcmp, strlen, memcpy

#include <regex.h>                                   // regexec

#include "corRest/CorRestState.h"                      // corRest
#include "corJsonld/corLdInit.h"                       // CORLD_CORE_CONTEXT_URL

#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeBuilder.h"                  // corTreeArray, corTreeObject, corTreeString, corTreeChildAdd
#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corJson/corJsonRender.h"                   // corJsonFastRender
#include "corJson/corJsonRenderSize.h"               // corJsonFastRenderSize
#include "corAlloc/corAlloc.h"                       // corAlloc

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/ldEntityFragment.h"               // ldEntityFragmentForInfo
#include "corNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieve, ldRegOpSupported
#include "corNgsild/ldDistOp.h"                       // ldDistOpSend, ldDistOpLoopDetected, ldDistOpCsrWouldLoop, ldDistOpBatchErrorAdd, ldDistOpForwardFailureReason
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant

#include "troe/TroeDriver.h"                         // troe
#include "troe/troeNotAvailable.h"                   // troeNotAvailable

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postEntityTemporalAttrs.h" // Own interface



static bool hasNonKeywordAttr(CorNode* entityP)
{
  if (entityP == NULL || entityP->type != CorObject)
    return false;
  for (CorNode* c = entityP->value.head; c != NULL; c = c->next)
  {
    if (c->name == NULL)             continue;
    if (c->name[0] == '@')           continue;
    if (strcmp(c->name, "id")   == 0) continue;
    if (strcmp(c->name, "type") == 0) continue;
    return true;
  }
  return false;
}



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



static char* renderFragmentWithContext(CorNode* fragP)
{
  // Strip body @context: forward goes out as application/json + Link.
  CorNode* atCtx = corTreeLookup(fragP, "@context");
  if (atCtx != NULL)
    corTreeChildRemove(fragP, atCtx);

  int   sz  = corJsonFastRenderSize(fragP) + 1;
  char* buf = (char*) corAlloc(&corRest.kalloc, sz);
  corJsonFastRender(fragP, buf);
  return buf;
}



bool postEntityTemporalAttrs(void)
{
  const char* entityId = corRest.in.wildcard[0];
  CorNode*    bodyP    = corRest.in.requestTree;

  if (entityId == NULL || entityId[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Missing URL Component", "missing entity id in URL");
    return true;
  }
  if (bodyP == NULL || bodyP->type != CorObject)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Not a JSON Object",
            "request body must be a JSON-LD object (EntityTemporal Fragment)");
    return true;
  }

  if (troe.entityTemporalAttrsAdd == NULL)
  {
    troeNotAvailable("temporal attribute add");
    return true;
  }

  Tenant* tenantP    = (Tenant*) corNgsild.tenantP;
  bool    inputHadAttrs = hasNonKeywordAttr(bodyP);

  CorNode* errorsArrayP = corTreeArray(corRest.kallocP, "errors");
  bool    anySucceeded = false;

  // Distop dispatch (§ 4.3.6 / § 5.6.12.4). No type known from URL — match
  // CSRs by id alone. Auxiliary mode is read-only — never enters the write
  // dispatch.
  if (!corNgsild.local && tenantP != NULL && tenantP->regCacheP != NULL)
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);
    bool        loopSeen = ldDistOpLoopDetected(ownAlias);

    if (!loopSeen)
    {
      LdRegMode modes[]      = { LdRegModeExclusive, LdRegModeRedirect, LdRegModeInclusive };
      bool      detachOnMode[] = { true, true, false };

      LdRegCacheItem** matchV[3] = { NULL, NULL, NULL };
      int              matchN[3] = { 0, 0, 0 };
      int              total     = 0;
      for (int m = 0; m < 3; m++)
      {
        matchN[m] = ldRegCacheMatchForRetrieve((LdRegCache*) tenantP->regCacheP,
                                               entityId, NULL, modes[m], &matchV[m]);
        total += matchN[m];
      }

      // upper-bound: total CSRs × max riP per CSR; allocate per riP encountered
      int riCap = 0;
      for (int m = 0; m < 3; m++)
        for (int i = 0; i < matchN[m]; i++)
          for (LdRegInfo* riP = matchV[m][i]->infoV; riP != NULL; riP = riP->next) riCap++;

      LdDistOpBatchItem*   items   = (LdDistOpBatchItem*)   corAlloc(&corRest.kalloc, riCap * sizeof(LdDistOpBatchItem));
      memset(items, 0, riCap * sizeof(LdDistOpBatchItem));
      LdDistOpBatchResult* results = (LdDistOpBatchResult*) corAlloc(&corRest.kalloc, riCap * sizeof(LdDistOpBatchResult));
      int                  itemCount = 0;
      memset(results, 0, riCap * sizeof(LdDistOpBatchResult));

      for (int m = 0; m < 3; m++)
      {
        for (int i = 0; i < matchN[m]; i++)
        {
          LdRegCacheItem* csr = matchV[m][i];
          if (csr->endpoint == NULL)                          continue;
          if (ldDistOpCsrWouldLoop(csr, ownAlias))            continue;

          bool opSupported = ldRegOpSupported(csr, LdOpAppendAttrsTemporal);

          for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
          {
            if (!entityInfoCoversId(riP, entityId)) continue;

            CorNode* fragP = ldEntityFragmentForInfo(bodyP, riP, corRest.kallocP, detachOnMode[m]);
            if (fragP == NULL) continue;

            if (!opSupported)
            {
              ldDistOpBatchErrorAdd(errorsArrayP, entityId, 409,
                                    LD_ERROR_CONFLICT, "Conflict",
                                    "registration does not support appendAttrsTemporal",
                                    csr->regId);
              continue;
            }

            const char* prefix = "/ngsi-ld/v1/temporal/entities/";
            const char* suffix = "/attrs/";   // trailing slash per spec URI template
            int baseLen = strlen(csr->endpoint);
            int prefLen = strlen(prefix);
            int idLen   = strlen(entityId);
            int sufLen  = strlen(suffix);
            char* url = (char*) corAlloc(&corRest.kalloc, baseLen + prefLen + idLen + sufLen + 1);
            int pos = 0;
            memcpy(url + pos, csr->endpoint, baseLen); pos += baseLen;
            memcpy(url + pos, prefix, prefLen);        pos += prefLen;
            memcpy(url + pos, entityId, idLen);        pos += idLen;
            memcpy(url + pos, suffix, sufLen);         pos += sufLen;
            url[pos] = 0;
            char* body = renderFragmentWithContext(fragP);

            items[itemCount].csr     = csr;
            items[itemCount].url     = url;
            items[itemCount].body    = body;
            items[itemCount].bodyLen = strlen(body);
            itemCount++;
          }
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

      for (int m = 0; m < 3; m++)
        ldRegCacheMatchRelease(matchV[m], matchN[m]);
    }
  }

  // Local TRoE append — skip when excl/redir consumed every input attr.
  bool distopsConsumedAll = (inputHadAttrs && !hasNonKeywordAttr(bodyP));

  if (!distopsConsumedAll)
  {
    int r = troe.entityTemporalAttrsAdd(tenantP, entityId, bodyP);

    if (r == TROE_NOT_FOUND)
    {
      if (!anySucceeded)
      {
        ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                "no temporal evolution for entity '%s'", entityId);
        return true;
      }
      // Forward(s) succeeded → swallow local 404 (the entity exists upstream).
    }
    else if (r != TROE_OK)
    {
      if (!anySucceeded)
      {
        ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
                "temporal add-attrs failed for '%s'", entityId);
        return true;
      }
      char detail[256];
      snprintf(detail, sizeof(detail),
               "local temporal add-attrs failed for '%s'", entityId);
      ldDistOpBatchErrorAdd(errorsArrayP, entityId, 500,
                            LD_ERROR_INTERNAL_ERROR, "Internal Error",
                            detail, NULL);
    }
    else
      anySucceeded = true;
  }

  int errorsCount = 0;
  for (CorNode* p = errorsArrayP->value.head; p != NULL; p = p->next) errorsCount++;

  if (errorsCount == 0)
  {
    corRest.out.httpStatusCode = 204;
    return true;
  }

  CorNode* result    = corTreeObject(corRest.kallocP, NULL);
  CorNode* successArr = corTreeArray(corRest.kallocP, "success");
  if (anySucceeded)
    corTreeChildAdd(successArr, corTreeString(corRest.kallocP, NULL, entityId));
  corTreeChildAdd(result, successArr);
  corTreeChildAdd(result, errorsArrayP);

  corRest.out.responseTree   = result;
  corRest.out.httpStatusCode = anySucceeded ? 207 : 502;
  return true;
}

//
// FILE            deleteEntityTemporalAttr.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// DELETE /ngsi-ld/v1/temporal/entities/{id}/attrs/{attr} (§ 5.6.13 / § 6.21.3.1).
// URL params:
//   ?datasetId=<uri>   — limit deletion to one dataset
//   ?deleteAll=true    — every instance regardless of datasetId
// Defaults: only the default-dataset (datasetId='') is deleted.
//
// Distops: broadcast DELETE to CSRs that cover the attr and support
// deleteAttrsTemporal.
//

#include <stddef.h>                                  // NULL
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // free
#include <string.h>                                  // strcmp, strlen, strcpy

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/corRestUrlValueEncode.h"             // corRestUrlValueEncode
#include "corJsonld/corLdInit.h"                       // corLdCoreContext
#include "corJsonld/corLdExpand.h"                     // corLdExpand
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjObject, kjString, kjChildAdd
#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/CorNgsild.h"                       // corNgsild fields
#include "corNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieve, ldRegOpSupported
#include "corNgsild/ldDistOp.h"                       // ldDistOpSend, ldDistOpLoopDetected, ldDistOpCsrWouldLoop, ldDistOpBatchErrorAdd, ldDistOpForwardFailureReason
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant

#include "troe/TroeDriver.h"                         // troe
#include "troe/troeNotAvailable.h"                   // troeNotAvailable

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/deleteEntityTemporalAttr.h"  // Own interface



static bool csrCoversAttr(LdRegCacheItem* csr, const char* attrIri)
{
  for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
  {
    if (riP->attributeNamesV == NULL)
      return true;
    for (int i = 0; riP->attributeNamesV[i] != NULL; i++)
      if (strcmp(riP->attributeNamesV[i], attrIri) == 0) return true;
  }
  return false;
}



bool deleteEntityTemporalAttr(void)
{
  const char* entityId = corRest.in.wildcard[0];
  const char* attrWild = corRest.in.wildcard[1];

  if (entityId == NULL || entityId[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Missing URL Component", "missing entity id in URL");
    return true;
  }
  if (attrWild == NULL || attrWild[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Missing URL Component", "missing attribute name in URL");
    return true;
  }

  if (troe.entityTemporalAttrDelete == NULL)
  {
    troeNotAvailable("temporal attribute delete");
    return true;
  }

  ldContextResolve();
  CorLdContext* ctxP    = (corNgsild.contextP != NULL) ? corNgsild.contextP : corLdCoreContext();
  const char*  attrIri = corLdExpand(ctxP, attrWild, &corRest.kalloc, NULL, NULL);
  if (attrIri == NULL) attrIri = attrWild;

  Tenant* tenantP = (Tenant*) corNgsild.tenantP;

  const char* datasetId = NULL;
  if (corNgsild.datasetIdV != NULL && corNgsild.datasetIdV[0] != NULL)
    datasetId = corNgsild.datasetIdV[0];

  // Forwarded query string mirrors the request's datasetId / deleteAll.
  // Build directly from the parsed corNgsild fields rather than echoing
  // raw URL params - which have been URL-DECODED by the time we see them,
  // so the datasetId (a URI, and free to carry any of them) is encoded again
  // on the way out.
  const char* datasetIdEnc = (datasetId != NULL) ? corRestUrlValueEncode(datasetId, &corRest.kalloc) : NULL;
  int         fwdQsSize    = ((datasetIdEnc != NULL) ? strlen(datasetIdEnc) : 0) + 64;

  char* fwdQs = (char*) kaAlloc(&corRest.kalloc, fwdQsSize);
  int   qpos  = 0;
  if (datasetIdEnc != NULL)
    qpos += snprintf(fwdQs + qpos, fwdQsSize - qpos, "datasetId=%s", datasetIdEnc);
  if (corNgsild.deleteAll)
  {
    if (qpos > 0) fwdQs[qpos++] = '&';
    qpos += snprintf(fwdQs + qpos, fwdQsSize - qpos, "deleteAll=true");
  }
  fwdQs[qpos] = 0;

  KjNode* errorsArrayP = kjArray(corRest.kjsonP, "errors");
  bool    anySucceeded = false;

  if (!corNgsild.local && tenantP != NULL && tenantP->regCacheP != NULL)
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);

    // Always dispatch; the builder marks loop-blocked CSRs and ldDistOpLoopReap emits 508 (§ 6.3.18).
    {
      LdRegMode modes[] = { LdRegModeExclusive, LdRegModeRedirect, LdRegModeInclusive };
      LdRegCacheItem** matchV[3] = { NULL, NULL, NULL };
      int              matchN[3] = { 0, 0, 0 };
      for (int m = 0; m < 3; m++)
        matchN[m] = ldRegCacheMatchForRetrieve((LdRegCache*) tenantP->regCacheP,
                                               entityId, NULL, modes[m], &matchV[m]);

      // Drop CSRs that don't cover this attribute — keeps the dispatch
      // one-per-csr without forcing the entries-builder to do per-RI walks.
      for (int m = 0; m < 3; m++)
      {
        int k = 0;
        for (int i = 0; i < matchN[m]; i++)
        {
          if (csrCoversAttr(matchV[m][i], attrIri)) matchV[m][k++] = matchV[m][i];
          else                                      ldRegCacheItemUnpin(matchV[m][i]);  // dropped — unpin now
        }
        matchN[m] = k;
      }

      LdDistOpGroup groups[] = {
        { matchV[0], matchN[0], "exclusive", false },
        { matchV[1], matchN[1], "redirect",  false },
        { matchV[2], matchN[2], "inclusive", false },
      };

      LdDistOpEntry* items;
      int n = ldDistOpEntriesBuild(groups, 3, ownAlias,
                                    LdOpDeleteAttrsTemporal, "deleteAttrsTemporal",
                                    entityId, /*perRi=*/false, NULL, NULL,
                                    errorsArrayP, &items);

      const char* prefix  = "/ngsi-ld/v1/temporal/entities/";
      const char* midSep  = "/attrs/";
      int         prefLen = strlen(prefix);
      int         midLen  = strlen(midSep);
      int         idLen   = strlen(entityId);
      int         atLen   = strlen(attrWild);
      int         qsLen   = (fwdQs != NULL && fwdQs[0] != 0) ? (int) strlen(fwdQs) : 0;
      for (int i = 0; i < n; i++)
      {
        int   baseLen = strlen(items[i].csr->endpoint);
        char* url     = (char*) kaAlloc(&corRest.kalloc, baseLen + prefLen + idLen + midLen + atLen + 1 + qsLen + 1);
        int pos = 0;
        memcpy(url + pos, items[i].csr->endpoint, baseLen); pos += baseLen;
        memcpy(url + pos, prefix, prefLen);                 pos += prefLen;
        memcpy(url + pos, entityId, idLen);                 pos += idLen;
        memcpy(url + pos, midSep, midLen);                  pos += midLen;
        memcpy(url + pos, attrWild, atLen);                 pos += atLen;
        if (qsLen > 0) { url[pos++] = '?'; memcpy(url + pos, fwdQs, qsLen); pos += qsLen; }
        url[pos] = 0;
        items[i].url = url;
      }

      n = ldDistOpLoopReap(items, n);

      ldDistOpEntriesPerform(items, n, CorVerbDelete, ownAlias);

      for (int i = 0; i < n; i++)
      {
        int sc = items[i].statusCode;
        if (sc == 404) continue;
        if (sc < 200 || sc >= 300)
          ldDistOpBatchErrorAdd(errorsArrayP, entityId, (sc >= 400) ? sc : 502,
                                LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                                ldDistOpForwardFailureReason(sc, items[i].errorDetail),
                                items[i].csr->regId);
        else
          anySucceeded = true;
      }

      for (int m = 0; m < 3; m++)
        ldRegCacheMatchRelease(matchV[m], matchN[m]);
    }
  }

  int r = troe.entityTemporalAttrDelete(tenantP, entityId, attrIri, datasetId, corNgsild.deleteAll);

  bool localOk       = (r == TROE_OK);
  bool localNotFound = (r == TROE_NOT_FOUND);

  if (localOk)
    anySucceeded = true;
  else if (!localNotFound && !anySucceeded)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "temporal attribute delete failed for '%s'/'%s'", entityId, attrWild);
    return true;
  }
  else if (!localOk && !localNotFound)
  {
    char detail[256];
    snprintf(detail, sizeof(detail),
             "local temporal attr delete failed for '%s'/'%s'", entityId, attrWild);
    ldDistOpBatchErrorAdd(errorsArrayP, entityId, 500,
                          LD_ERROR_INTERNAL_ERROR, "Internal Error",
                          detail, NULL);
  }

  if (!anySucceeded && localNotFound)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "no temporal data for entity '%s' / attribute '%s'", entityId, attrWild);
    return true;
  }

  int errorsCount = 0;
  for (KjNode* p = errorsArrayP->value.firstChildP; p != NULL; p = p->next) errorsCount++;

  if (errorsCount == 0)
  {
    corRest.out.httpStatusCode = 204;
    return true;
  }

  KjNode* result     = kjObject(corRest.kjsonP, NULL);
  KjNode* successArr = kjArray(corRest.kjsonP, "success");
  if (anySucceeded)
    kjChildAdd(successArr, kjString(corRest.kjsonP, NULL, entityId));
  kjChildAdd(result, successArr);
  kjChildAdd(result, errorsArrayP);

  corRest.out.responseTree   = result;
  corRest.out.httpStatusCode = anySucceeded ? 207 : 502;
  return true;
}

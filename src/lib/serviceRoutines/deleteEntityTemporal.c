//
// FILE            deleteEntityTemporal.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// DELETE /ngsi-ld/v1/temporal/entities/{id} — § 5.6.16 / § 6.19.3.2.
// Removes the complete temporal evolution of one entity.
//
// Distops (§ 4.3.6 / § 5.6.16.4): broadcast DELETE to every CSR that
// matches by id and supports deleteTemporal. Per § 4.20 Table 4.20-2,
// deleteTemporal is NOT in any default group — CSRs must opt in.
//

#include <stddef.h>                                  // NULL
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // free
#include <string.h>                                  // strlen, strcpy

#include "corRest/CorRestState.h"                      // corRest
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjObject, kjString, kjChildAdd
#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieve, ldRegOpSupported
#include "corNgsild/ldDistOp.h"                       // ldDistOpSend, ldDistOpLoopDetected, ldDistOpCsrWouldLoop, ldDistOpBatchErrorAdd, ldDistOpForwardFailureReason
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant

#include "troe/TroeDriver.h"                         // troe
#include "troe/troeNotAvailable.h"                   // troeNotAvailable

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/deleteEntityTemporal.h"    // Own interface



static int forwardDeleteTemporal(LdRegCacheItem* csr,
                                 const char*     entityId,
                                 const char*     ownAlias,
                                 const char**    errorDetailPP)
{
  const char* path    = "/ngsi-ld/v1/temporal/entities/";
  int         baseLen = strlen(csr->endpoint);
  int         pathLen = strlen(path);
  int         idLen   = strlen(entityId);
  char*       url     = (char*) kaAlloc(&corRest.kalloc, baseLen + pathLen + idLen + 1);
  strcpy(url, csr->endpoint);
  strcpy(url + baseLen, path);
  strcpy(url + baseLen + pathLen, entityId);

  return ldDistOpSend(csr, CorVerbDelete, url, NULL, 0, ownAlias, errorDetailPP);
}



bool deleteEntityTemporal(void)
{
  const char* entityId = corRest.in.wildcard[0];

  if (entityId == NULL || entityId[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Missing URL Component", "missing entity id in URL");
    return true;
  }

  if (troe.entityTemporalDelete == NULL)
  {
    troeNotAvailable("temporal-entity delete");
    return true;
  }

  Tenant* tenantP = (Tenant*) corNgsild.tenantP;

  KjNode* errorsArrayP = kjArray(corRest.kjsonP, "errors");
  bool    anySucceeded = false;

  // Distop dispatch — broadcast to every matching CSR (3 modes; auxiliary
  // is read-only). Type unknown from URL; let the cache match by id alone.
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

      LdDistOpGroup groups[] = {
        { matchV[0], matchN[0], "exclusive", false },
        { matchV[1], matchN[1], "redirect",  false },
        { matchV[2], matchN[2], "inclusive", false },
      };

      LdDistOpEntry* items;
      int n = ldDistOpEntriesBuild(groups, 3, ownAlias,
                                    LdOpDeleteTemporal, "deleteTemporal",
                                    entityId, /*perRi=*/false, NULL, NULL,
                                    errorsArrayP, &items);

      const char* path    = "/ngsi-ld/v1/temporal/entities/";
      int         pathLen = strlen(path);
      int         idLen   = strlen(entityId);
      for (int i = 0; i < n; i++)
      {
        int   baseLen = strlen(items[i].csr->endpoint);
        char* url     = (char*) kaAlloc(&corRest.kalloc, baseLen + pathLen + idLen + 1);
        strcpy(url, items[i].csr->endpoint);
        strcpy(url + baseLen, path);
        strcpy(url + baseLen + pathLen, entityId);
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

  int r = troe.entityTemporalDelete(tenantP, entityId);

  bool localOk         = (r == TROE_OK);
  bool localNotFound   = (r == TROE_NOT_FOUND);

  if (localOk)
    anySucceeded = true;
  else if (!localNotFound && r != TROE_OK && !anySucceeded)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "temporal-entity delete failed for '%s'", entityId);
    return true;
  }
  else if (!localOk && !localNotFound)
  {
    char detail[256];
    snprintf(detail, sizeof(detail),
             "local temporal delete failed for entity '%s'", entityId);
    ldDistOpBatchErrorAdd(errorsArrayP, entityId, 500,
                          LD_ERROR_INTERNAL_ERROR, "Internal Error",
                          detail, NULL);
  }

  // Both legs found nothing → 404.
  if (!anySucceeded && localNotFound)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "no temporal data for entity '%s'", entityId);
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

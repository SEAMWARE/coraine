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

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjObject, kjString, kjChildAdd
#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieve, ldRegOpSupported
#include "swNgsild/ldDistOp.h"                       // ldDistOpSend, ldDistOpLoopDetected, ldDistOpCsrWouldLoop, ldDistOpBatchErrorAdd, ldDistOpForwardFailureReason
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant

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
  char*       url     = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + idLen + 1);
  strcpy(url, csr->endpoint);
  strcpy(url + baseLen, path);
  strcpy(url + baseLen + pathLen, entityId);

  return ldDistOpSend(csr, SwVerbDelete, url, NULL, 0, ownAlias, errorDetailPP);
}



bool deleteEntityTemporal(void)
{
  const char* entityId = swRest.in.wildcard[0];

  if (entityId == NULL || entityId[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "missing entity id in URL");
    return true;
  }

  if (troe.entityTemporalDelete == NULL)
  {
    troeNotAvailable("temporal-entity delete");
    return true;
  }

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  KjNode* errorsArrayP = kjArray(swRest.kjsonP, "errors");
  bool    anySucceeded = false;

  // Distop dispatch — broadcast to every matching CSR (3 modes; auxiliary
  // is read-only). Type unknown from URL; let the cache match by id alone.
  if (!swNgsild.local && tenantP != NULL && tenantP->regCacheP != NULL)
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

    if (!ldDistOpLoopDetected(ownAlias))
    {
      LdRegMode modes[] = { LdRegModeExclusive, LdRegModeRedirect, LdRegModeInclusive };
      LdRegCacheItem** matchV[3] = { NULL, NULL, NULL };
      int              matchN[3] = { 0, 0, 0 };
      int              total     = 0;
      for (int m = 0; m < 3; m++)
      {
        matchN[m] = ldRegCacheMatchForRetrieve((LdRegCache*) tenantP->regCacheP,
                                               entityId, NULL, modes[m], &matchV[m]);
        total += matchN[m];
      }

      LdDistOpBatchItem*   items   = (LdDistOpBatchItem*)   kaAlloc(&swRest.kalloc, total * sizeof(LdDistOpBatchItem));
      LdDistOpBatchResult* results = (LdDistOpBatchResult*) kaAlloc(&swRest.kalloc, total * sizeof(LdDistOpBatchResult));
      int                  itemCount = 0;
      memset(results, 0, total * sizeof(LdDistOpBatchResult));

      for (int m = 0; m < 3; m++)
      {
        for (int i = 0; i < matchN[m]; i++)
        {
          LdRegCacheItem* csr = matchV[m][i];
          if (csr->endpoint == NULL)               continue;
          if (ldDistOpCsrWouldLoop(csr, ownAlias)) continue;
          if (!ldRegOpSupported(csr, LdOpDeleteTemporal)) continue;

          int idLen   = strlen(entityId);
          int baseLen = strlen(csr->endpoint);
          const char* path = "/ngsi-ld/v1/temporal/entities/";
          int pathLen = strlen(path);
          char* url = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + idLen + 1);
          strcpy(url, csr->endpoint);
          strcpy(url + baseLen, path);
          strcpy(url + baseLen + pathLen, entityId);

          items[itemCount].csr     = csr;
          items[itemCount].url     = url;
          items[itemCount].body    = NULL;
          items[itemCount].bodyLen = 0;
          itemCount++;
        }
      }

      if (itemCount > 0)
      {
        ldDistOpSendMulti(items, itemCount, SwVerbDelete, ownAlias, results);

        for (int i = 0; i < itemCount; i++)
        {
          int upCode = results[i].statusCode;
          if (upCode == 404) continue;
          if (upCode < 200 || upCode >= 300)
            ldDistOpBatchErrorAdd(errorsArrayP, entityId,
                                  LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                                  ldDistOpForwardFailureReason(upCode, results[i].errorDetail),
                                  items[i].csr->regId);
          else
            anySucceeded = true;
        }
      }

      for (int m = 0; m < 3; m++)
        if (matchV[m] != NULL) free(matchV[m]);
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
    ldDistOpBatchErrorAdd(errorsArrayP, entityId,
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
    swRest.out.httpStatusCode = 204;
    return true;
  }

  KjNode* result     = kjObject(swRest.kjsonP, NULL);
  KjNode* successArr = kjArray(swRest.kjsonP, "success");
  if (anySucceeded)
    kjChildAdd(successArr, kjString(swRest.kjsonP, NULL, entityId));
  kjChildAdd(result, successArr);
  kjChildAdd(result, errorsArrayP);

  swRest.out.responseTree   = result;
  swRest.out.httpStatusCode = anySucceeded ? 207 : 502;
  return true;
}

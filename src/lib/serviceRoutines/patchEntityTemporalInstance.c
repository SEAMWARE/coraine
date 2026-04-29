//
// FILE            patchEntityTemporalInstance.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// PATCH /ngsi-ld/v1/temporal/entities/{id}/attrs/{attr}/{instance}
// (§ 5.6.14 / § 6.22.3.1) — Modify a single Attribute instance.
//
// Distops: broadcast PATCH to CSRs that cover the attr and support
// updateAttrInstanceTemporal.
//

#include <stddef.h>                                  // NULL
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // free
#include <string.h>                                  // strcmp, strlen, memcpy

#include "swRest/SwRestState.h"                      // swRest
#include "swJsonld/swldInit.h"                       // swldCoreContext, SWLD_CORE_CONTEXT_URL
#include "swJsonld/swldExpand.h"                     // swldExpand

#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjObject, kjString, kjChildAdd
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjClone.h"                           // kjClone
#include "kjson/kjRender.h"                          // kjFastRender
#include "kjson/kjRenderSize.h"                      // kjFastRenderSize
#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieve, ldRegOpSupported
#include "swNgsild/ldDistOp.h"                       // ldDistOpSend, ldDistOpLoopDetected, ldDistOpCsrWouldLoop, ldDistOpBatchErrorAdd, ldDistOpForwardFailureReason
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant

#include "troe/TroeDriver.h"                         // troe

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/patchEntityTemporalInstance.h"  // Own interface



static bool csrCoversAttr(LdRegCacheItem* csr, const char* attrIri)
{
  for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
  {
    if (riP->propertyNamesV == NULL && riP->relationshipNamesV == NULL)
      return true;
    if (riP->propertyNamesV != NULL)
      for (int i = 0; riP->propertyNamesV[i] != NULL; i++)
        if (strcmp(riP->propertyNamesV[i], attrIri) == 0) return true;
    if (riP->relationshipNamesV != NULL)
      for (int i = 0; riP->relationshipNamesV[i] != NULL; i++)
        if (strcmp(riP->relationshipNamesV[i], attrIri) == 0) return true;
  }
  return false;
}



static char* renderBodyWithContext(KjNode* bodyP)
{
  // Clone first — local TRoE plugin gets bodyP after this and can't
  // tolerate an @context child where it expects only value-bearing fields.
  KjNode* cloneP = kjClone(swRest.kjsonP, bodyP);
  if (kjLookup(cloneP, "@context") == NULL)
    kjChildAdd(cloneP, kjString(swRest.kjsonP, "@context", SWLD_CORE_CONTEXT_URL));

  int   sz  = kjFastRenderSize(cloneP) + 1;
  char* buf = (char*) kaAlloc(&swRest.kalloc, sz);
  kjFastRender(cloneP, buf);
  return buf;
}



static int forwardPatchInstance(LdRegCacheItem* csr,
                                const char*     entityId,
                                const char*     attrName,
                                const char*     instanceId,
                                const char*     body,
                                int             bodyLen,
                                const char*     ownAlias,
                                const char**    errorDetailPP)
{
  const char* prefix = "/ngsi-ld/v1/temporal/entities/";
  const char* mid    = "/attrs/";
  int   baseLen = strlen(csr->endpoint);
  int   prefLen = strlen(prefix);
  int   idLen   = strlen(entityId);
  int   midLen  = strlen(mid);
  int   atLen   = strlen(attrName);
  int   inLen   = strlen(instanceId);

  char* url = (char*) kaAlloc(&swRest.kalloc, baseLen + prefLen + idLen + midLen + atLen + 1 + inLen + 1);
  int   pos = 0;
  memcpy(url + pos, csr->endpoint, baseLen); pos += baseLen;
  memcpy(url + pos, prefix, prefLen);        pos += prefLen;
  memcpy(url + pos, entityId, idLen);        pos += idLen;
  memcpy(url + pos, mid, midLen);            pos += midLen;
  memcpy(url + pos, attrName, atLen);        pos += atLen;
  url[pos++] = '/';
  memcpy(url + pos, instanceId, inLen);      pos += inLen;
  url[pos] = 0;

  return ldDistOpSend(csr, SwVerbPatch, url, body, bodyLen, ownAlias, errorDetailPP);
}



bool patchEntityTemporalInstance(void)
{
  const char* entityId   = swRest.in.wildcard[0];
  const char* attrWild   = swRest.in.wildcard[1];
  const char* instanceId = swRest.in.wildcard[2];
  KjNode*     bodyP      = swRest.in.requestTree;

  if (entityId == NULL || entityId[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "missing entity id in URL");
    return true;
  }
  if (attrWild == NULL || attrWild[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "missing attribute name in URL");
    return true;
  }
  if (instanceId == NULL || instanceId[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "missing instance id in URL");
    return true;
  }
  if (bodyP == NULL || bodyP->type != KjObject)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "request body must be a JSON-LD object (EntityTemporal Fragment)");
    return true;
  }

  if (troe.entityTemporalInstanceModify == NULL)
  {
    ldError(422, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported",
            "Not Implemented",
            "active TRoE plugin does not support instance modify");
    return true;
  }

  ldContextResolve();
  SwldContext* ctxP    = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();
  const char*  attrIri = swldExpand(ctxP, attrWild, &swRest.kalloc, NULL, NULL);
  if (attrIri == NULL) attrIri = attrWild;

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  KjNode* errorsArrayP = kjArray(swRest.kjsonP, "errors");
  bool    anySucceeded = false;

  if (!swNgsild.local && tenantP != NULL && tenantP->regCacheP != NULL)
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

    if (!ldDistOpLoopDetected(ownAlias))
    {
      char* fwdBody    = renderBodyWithContext(bodyP);
      int   fwdBodyLen = strlen(fwdBody);

      LdRegMode modes[] = { LdRegModeExclusive, LdRegModeRedirect, LdRegModeInclusive };
      for (int m = 0; m < 3; m++)
      {
        LdRegCacheItem** matchV = NULL;
        int matchN = ldRegCacheMatchForRetrieve((LdRegCache*) tenantP->regCacheP,
                                                entityId, NULL, modes[m], &matchV);

        for (int i = 0; i < matchN; i++)
        {
          LdRegCacheItem* csr = matchV[i];
          if (csr->endpoint == NULL)                                  continue;
          if (ldDistOpCsrWouldLoop(csr, ownAlias))                    continue;
          if (!ldRegOpSupported(csr, LdOpUpdateAttrInstanceTemporal)) continue;
          if (!csrCoversAttr(csr, attrIri))                           continue;

          const char* upErr  = NULL;
          int         upCode = forwardPatchInstance(csr, entityId, attrWild, instanceId,
                                                    fwdBody, fwdBodyLen, ownAlias, &upErr);

          if (upCode == 404)
            continue;
          if (upCode < 200 || upCode >= 300)
            ldDistOpBatchErrorAdd(errorsArrayP, entityId,
                                  LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                                  ldDistOpForwardFailureReason(upCode, upErr),
                                  csr->regId);
          else
            anySucceeded = true;
        }

        if (matchV != NULL) free(matchV);
      }
    }
  }

  int r = troe.entityTemporalInstanceModify(tenantP, entityId, attrIri, instanceId, bodyP);

  bool localOk       = (r == TROE_OK);
  bool localNotFound = (r == TROE_NOT_FOUND);

  if (localOk)
    anySucceeded = true;
  else if (!localNotFound && !anySucceeded)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "temporal instance modify failed");
    return true;
  }
  else if (!localOk && !localNotFound)
  {
    char detail[256];
    snprintf(detail, sizeof(detail),
             "local instance modify failed for '%s'/'%s'/'%s'",
             entityId, attrWild, instanceId);
    ldDistOpBatchErrorAdd(errorsArrayP, entityId,
                          LD_ERROR_INTERNAL_ERROR, "Internal Error",
                          detail, NULL);
  }

  if (!anySucceeded && localNotFound)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "no temporal instance '%s' for entity '%s' / attribute '%s'",
            instanceId, entityId, attrWild);
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

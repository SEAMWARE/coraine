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

#include "corRest/CorRestState.h"                      // corRest
#include "corJsonld/corLdInit.h"                       // corLdCoreContext, CORLD_CORE_CONTEXT_URL
#include "corJsonld/corLdExpand.h"                     // corLdExpand

#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjObject, kjString, kjChildAdd, kjChildRemove
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjClone.h"                           // kjClone
#include "kjson/kjRender.h"                          // kjFastRender
#include "kjson/kjRenderSize.h"                      // kjFastRenderSize
#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieve, ldRegOpSupported
#include "corNgsild/ldDistOp.h"                       // ldDistOpSend, ldDistOpLoopDetected, ldDistOpCsrWouldLoop, ldDistOpBatchErrorAdd, ldDistOpForwardFailureReason
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant

#include "troe/TroeDriver.h"                         // troe
#include "troe/troeNotAvailable.h"                   // troeNotAvailable

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/patchEntityTemporalInstance.h"  // Own interface



// -----------------------------------------------------------------------------
//
// bodyIsBareInstance - is the body the Attribute instance itself, unwrapped?
//
// The spec's fragment is { "<attr>": [ { instance } ] }, but a bare instance
// body has long been accepted too. It carries no Attribute name, so there is
// nothing in it that could disagree with the one in the URL — recognised the
// same way the TRoE plugins recognise it, by a known Attribute type.
//
static bool bodyIsBareInstance(KjNode* bodyP)
{
  KjNode* tP = kjLookup(bodyP, "type");

  if (tP == NULL || tP->type != KjString || tP->value.s == NULL)
    return false;

  const char* t = tP->value.s;

  return ((strcmp(t, "Property")         == 0) || (strcmp(t, "Relationship")     == 0) ||
          (strcmp(t, "GeoProperty")      == 0) || (strcmp(t, "LanguageProperty") == 0) ||
          (strcmp(t, "VocabProperty")    == 0) || (strcmp(t, "ListProperty")     == 0) ||
          (strcmp(t, "ListRelationship") == 0) || (strcmp(t, "JsonProperty")     == 0));
}



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



static char* renderBodyWithContext(KjNode* bodyP)
{
  // Clone first — local TRoE plugin gets bodyP after this and can't
  // tolerate an @context child where it expects only value-bearing fields.
  KjNode* cloneP = kjClone(corRest.kjsonP, bodyP);
  // Strip body @context: forward goes out as application/json + Link.
  KjNode* atCtx = kjLookup(cloneP, "@context");
  if (atCtx != NULL)
    kjChildRemove(cloneP, atCtx);

  int   sz  = kjFastRenderSize(cloneP) + 1;
  char* buf = (char*) kaAlloc(&corRest.kalloc, sz);
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

  char* url = (char*) kaAlloc(&corRest.kalloc, baseLen + prefLen + idLen + midLen + atLen + 1 + inLen + 1);
  int   pos = 0;
  memcpy(url + pos, csr->endpoint, baseLen); pos += baseLen;
  memcpy(url + pos, prefix, prefLen);        pos += prefLen;
  memcpy(url + pos, entityId, idLen);        pos += idLen;
  memcpy(url + pos, mid, midLen);            pos += midLen;
  memcpy(url + pos, attrName, atLen);        pos += atLen;
  url[pos++] = '/';
  memcpy(url + pos, instanceId, inLen);      pos += inLen;
  url[pos] = 0;

  return ldDistOpSend(csr, CorVerbPatch, url, body, bodyLen, ownAlias, errorDetailPP);
}



bool patchEntityTemporalInstance(void)
{
  const char* entityId   = corRest.in.wildcard[0];
  const char* attrWild   = corRest.in.wildcard[1];
  const char* instanceId = corRest.in.wildcard[2];
  KjNode*     bodyP      = corRest.in.requestTree;

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
  if (instanceId == NULL || instanceId[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Missing URL Component", "missing instance id in URL");
    return true;
  }
  if (bodyP == NULL || bodyP->type != KjObject)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Not a JSON Object",
            "request body must be a JSON-LD object (EntityTemporal Fragment)");
    return true;
  }

  if (troe.entityTemporalInstanceModify == NULL)
  {
    troeNotAvailable("temporal-instance modify");
    return true;
  }

  ldContextResolve();
  CorLdContext* ctxP    = (corNgsild.contextP != NULL) ? corNgsild.contextP : corLdCoreContext();
  const char*  attrIri = corLdExpand(ctxP, attrWild, &corRest.kalloc, NULL, NULL);
  if (attrIri == NULL) attrIri = attrWild;

  //
  // The body is an EntityTemporal Fragment, and § 11.2.5.4 says to replace the
  // target instance with "the Attribute instance in the EntityTemporal
  // Fragment" — the instance held under the TARGET Attribute's name. Names are
  // compared expanded (§ 8.2.4), so "speed" and its IRI are the same target.
  //
  // Without this check the fragment's Attribute name was ignored entirely and
  // whichever array came first was applied: PATCH .../attrs/speed/{inst} with a
  // body of {"color":[...]} silently overwrote the speed instance with colour
  // data. § 11.2.5.3 also fixes the cardinality — "an Array of exactly one
  // item" — and a second item used to be dropped without a word.
  //
  // A bare instance body (no Attribute wrapper) stays accepted as before; there
  // is no name in it that could contradict the URL.
  //
  if (!bodyIsBareInstance(bodyP))
  {
    KjNode* targetP = NULL;

    for (KjNode* fP = bodyP->value.firstChildP; fP != NULL; fP = fP->next)
    {
      if (fP->name == NULL)               continue;
      if (fP->name[0] == '@')             continue;
      if (strcmp(fP->name, "id")   == 0)  continue;
      if (strcmp(fP->name, "type") == 0)  continue;

      const char* fIri = corLdExpand(ctxP, fP->name, &corRest.kalloc, NULL, NULL);
      if (fIri == NULL) fIri = fP->name;

      if (strcmp(fIri, attrIri) == 0)
      {
        targetP = fP;
        break;
      }
    }

    if (targetP == NULL)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Fragment",
              "EntityTemporal Fragment does not contain the target attribute '%s'", attrWild);
      return true;
    }

    if (targetP->type == KjArray)
    {
      int instances = 0;
      for (KjNode* iP = targetP->value.firstChildP; iP != NULL; iP = iP->next)
        instances++;

      if (instances != 1)
      {
        ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Fragment",
                "attribute '%s' must hold exactly one instance, got %d", attrWild, instances);
        return true;
      }
    }

    //
    // Drop every other Attribute member, so the target instance is the only one
    // the TRoE plugin can pick up regardless of the order they arrived in.
    //
    KjNode* fP = bodyP->value.firstChildP;
    while (fP != NULL)
    {
      KjNode* nextP = fP->next;

      if ((fP != targetP) && (fP->name != NULL) && (fP->name[0] != '@') &&
          (strcmp(fP->name, "id") != 0) && (strcmp(fP->name, "type") != 0))
        kjChildRemove(bodyP, fP);

      fP = nextP;
    }
  }

  Tenant* tenantP = (Tenant*) corNgsild.tenantP;

  KjNode* errorsArrayP = kjArray(corRest.kjsonP, "errors");
  bool    anySucceeded = false;

  if (!corNgsild.local && tenantP != NULL && tenantP->regCacheP != NULL)
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);

    // Always dispatch; the builder marks loop-blocked CSRs and ldDistOpLoopReap emits 508 (§ 6.3.18).
    {
      char* fwdBody    = renderBodyWithContext(bodyP);
      int   fwdBodyLen = strlen(fwdBody);

      LdRegMode modes[] = { LdRegModeExclusive, LdRegModeRedirect, LdRegModeInclusive };
      LdRegCacheItem** matchV[3] = { NULL, NULL, NULL };
      int              matchN[3] = { 0, 0, 0 };
      for (int m = 0; m < 3; m++)
        matchN[m] = ldRegCacheMatchForRetrieve((LdRegCache*) tenantP->regCacheP,
                                               entityId, NULL, modes[m], &matchV[m]);

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
                                    LdOpUpdateAttrInstanceTemporal, "updateAttrInstanceTemporal",
                                    entityId, /*perRi=*/false, NULL, NULL,
                                    errorsArrayP, &items);

      const char* prefix  = "/ngsi-ld/v1/temporal/entities/";
      const char* midSep  = "/attrs/";
      int         prefLen = strlen(prefix);
      int         midLen  = strlen(midSep);
      int         idLen   = strlen(entityId);
      int         atLen   = strlen(attrWild);
      int         inLen   = strlen(instanceId);
      for (int i = 0; i < n; i++)
      {
        int   baseLen = strlen(items[i].csr->endpoint);
        char* url     = (char*) kaAlloc(&corRest.kalloc, baseLen + prefLen + idLen + midLen + atLen + 1 + inLen + 1);
        int pos = 0;
        memcpy(url + pos, items[i].csr->endpoint, baseLen); pos += baseLen;
        memcpy(url + pos, prefix, prefLen);                 pos += prefLen;
        memcpy(url + pos, entityId, idLen);                 pos += idLen;
        memcpy(url + pos, midSep, midLen);                  pos += midLen;
        memcpy(url + pos, attrWild, atLen);                 pos += atLen;
        url[pos++] = '/';
        memcpy(url + pos, instanceId, inLen);               pos += inLen;
        url[pos] = 0;
        items[i].url     = url;
        items[i].body    = fwdBody;
        items[i].bodyLen = fwdBodyLen;
      }

      n = ldDistOpLoopReap(items, n);

      ldDistOpEntriesPerform(items, n, CorVerbPatch, ownAlias);

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
    ldDistOpBatchErrorAdd(errorsArrayP, entityId, 500,
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

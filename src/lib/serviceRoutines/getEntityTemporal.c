//
// FILE            getEntityTemporal.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/temporal/entities/{id}
// NGSI-LD § 5.7.3 — Retrieve Temporal Evolution of an Entity (§ 6.19.3.1).
//
// Filtering supported in this slice:
//   ?timerel + ?timeAt (+ ?endTimeAt for between)
//   ?timeproperty
//   ?attrs (comma list, post-expansion)
//   ?lastN (per-attribute instance cap)
//   ?datasetId
//
// Distops (§ 4.3.6 / § 5.7.5): forward to CSRs whose operations[] include
// "retrieveTemporal". Local TRoE result is merged with each CSR response
// — for the temporal API "merge" means concatenating per-attr instance
// arrays (each attr is an array of instances, not a single attribute
// object).
//

#include <stddef.h>                                  // NULL
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // free
#include <string.h>                                  // strcmp, memset, strlen, strcpy

#include "swRest/SwRestState.h"                      // swRest
#include "swRest/swRestOutHeader.h"                  // swRestOutHeaderAdd
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjChildAdd
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjParse.h"                           // kjParse
#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "swJsonld/swldExpandTree.h"                 // swldExpandTree

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdVocab.h"                        // LD_VOCAB_CREATED_AT, LD_VOCAB_MODIFIED_AT, LD_VOCAB_EXPIRES_AT
#include "swNgsild/ldParamsValidate.h"               // ldParamsValidate
#include "swNgsild/ldPickOmit.h"                     // ldPickOmit
#include "swNgsild/ldToTemporalValues.h"             // ldToTemporalValues
#include "swNgsild/ldStripAtContext.h"               // ldStripAtContext
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieve, ldRegOpSupported
#include "swNgsild/ldDistOp.h"                       // ldDistOpSendReceive, ldDistOpLoopDetected, ldDistOpCsrWouldLoop
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant

#include "troe/TroeDriver.h"                         // troe, TroeQueryFilter, TroeRangeInfo

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/ldSnapshotRead.h"          // ldSnapshotItemFromHeader
#include "serviceRoutines/getEntityTemporal.h"       // Own interface



// -----------------------------------------------------------------------------
//
// stripInfoAttrsFromTemporal - remove from localP every attr covered by one
// RegistrationInfo. Same shape rules as the regular variant in getEntity.c —
// removes top-level non-keyword attrs by name, regardless of whether each
// attr is a KjArray of instances (temporal) or a KjObject (storage-format).
//
static void stripInfoAttrsFromTemporal(KjNode* localP, LdRegInfo* riP)
{
  if (localP == NULL || localP->type != KjObject)
    return;

  bool wildcard = (riP->propertyNamesV == NULL && riP->relationshipNamesV == NULL);

  KjNode* curP = localP->value.firstChildP;
  while (curP != NULL)
  {
    KjNode* nextP = curP->next;

    if (curP->name != NULL && curP->name[0] != '@' &&
        strcmp(curP->name, "id")   != 0 &&
        strcmp(curP->name, "type") != 0)
    {
      bool covered = wildcard;
      if (!covered && riP->propertyNamesV != NULL)
        for (int j = 0; riP->propertyNamesV[j] != NULL; j++)
          if (strcmp(curP->name, riP->propertyNamesV[j]) == 0) { covered = true; break; }
      if (!covered && riP->relationshipNamesV != NULL)
        for (int j = 0; riP->relationshipNamesV[j] != NULL; j++)
          if (strcmp(curP->name, riP->relationshipNamesV[j]) == 0) { covered = true; break; }

      if (covered)
        kjChildRemove(localP, curP);
    }

    curP = nextP;
  }
}



// -----------------------------------------------------------------------------
//
// mergeTemporalInto - concatenate upstream's per-attr instance arrays into destP.
//
// EntityTemporal shape: each non-keyword child of the entity is a KjArray of
// instance objects. Merging is "for each attr in upP, append its instances
// to destP's same-named attr (creating it if absent)".
//
// keepOnlyMissing = true → auxiliary mode: only copy attrs that destP doesn't
// already have. Per § 4.3.6.2 auxiliary registrations fill gaps; they never
// overwrite or augment data already present locally.
//
static void mergeTemporalInto(KjNode* destP, KjNode* upP, bool keepOnlyMissing)
{
  if (destP == NULL || upP == NULL || destP->type != KjObject || upP->type != KjObject)
    return;

  KjNode* upChild = upP->value.firstChildP;
  while (upChild != NULL)
  {
    KjNode* upNext = upChild->next;

    if (upChild->name == NULL ||
        upChild->name[0] == '@' ||
        strcmp(upChild->name, "id")   == 0 ||
        strcmp(upChild->name, "type") == 0)
    {
      upChild = upNext;
      continue;
    }

    KjNode* destAttr = kjLookup(destP, upChild->name);

    if (destAttr == NULL)
    {
      // First time we see this attr — move the whole upstream array across.
      upChild->next = NULL;
      kjChildAdd(destP, upChild);
    }
    else if (!keepOnlyMissing && upChild->type == KjArray && destAttr->type == KjArray)
    {
      // Concat upstream instances onto destP's existing array.
      KjNode* inst = upChild->value.firstChildP;
      while (inst != NULL)
      {
        KjNode* instNext = inst->next;
        inst->next = NULL;
        kjChildAdd(destAttr, inst);
        inst = instNext;
      }
    }
    // else: auxiliary + already present → drop upstream silently.

    upChild = upNext;
  }
}



// -----------------------------------------------------------------------------
//
// buildTemporalForwardQuery - rebuild only the temporal-filter URL params,
// skipping output-shaping params (pick/omit/lang/format/sysAttrs/...) since
// the broker applies those on the merged result. Forwards: timerel, timeAt,
// endTimeAt, timeproperty, attrs, lastN, datasetId.
//
static const char* buildTemporalForwardQuery(KAlloc* kaP)
{
  static const char* forwardKeys[] = {
    "timerel", "timeAt", "endTimeAt", "timeproperty",
    "attrs", "lastN", "datasetId",
    NULL
  };

  // Worst-case length: each (key=value&) plus NULL terminator.
  int len = 1;
  for (int i = 0; i < swRest.in.uriParamCount; i++)
  {
    const char* key = swRest.in.uriParamV[i].key;
    bool wanted = false;
    for (int k = 0; forwardKeys[k] != NULL; k++)
      if (strcmp(key, forwardKeys[k]) == 0) { wanted = true; break; }
    if (!wanted) continue;

    const char* val = swRest.in.uriParamV[i].value;
    len += strlen(key) + 1 + (val ? strlen(val) : 0) + 1;  // key=val&
  }

  char* buf = (char*) kaAlloc(kaP, len);
  buf[0] = 0;
  int pos = 0;

  for (int i = 0; i < swRest.in.uriParamCount; i++)
  {
    const char* key = swRest.in.uriParamV[i].key;
    bool wanted = false;
    for (int k = 0; forwardKeys[k] != NULL; k++)
      if (strcmp(key, forwardKeys[k]) == 0) { wanted = true; break; }
    if (!wanted) continue;

    const char* val = swRest.in.uriParamV[i].value;
    if (pos > 0) buf[pos++] = '&';
    int kl = strlen(key);
    memcpy(buf + pos, key, kl);
    pos += kl;
    buf[pos++] = '=';
    if (val != NULL)
    {
      int vl = strlen(val);
      memcpy(buf + pos, val, vl);
      pos += vl;
    }
  }

  buf[pos] = 0;
  return buf;
}



// -----------------------------------------------------------------------------
//
// forwardTemporalAndParse - GET /temporal/entities/{id} from a CSR.
//
// Returns the parsed EntityTemporal tree (already JSON-LD expanded, with
// @context stripped) on success. Status code is the upstream HTTP status
// (or 502 for transport errors). On non-2xx, *upstreamPP is NULL.
//
static int forwardTemporalAndParse(LdRegCacheItem* csr,
                                   const char*     entityId,
                                   const char*     queryString,
                                   const char*     ownAlias,
                                   KjNode**        upstreamPP,
                                   const char**    errorDetailPP)
{
  *upstreamPP    = NULL;
  *errorDetailPP = NULL;

  const char* base = csr->endpoint;
  const char* path = "/ngsi-ld/v1/temporal/entities/";
  int baseLen = strlen(base);
  int pathLen = strlen(path);
  int idLen   = strlen(entityId);
  int qsLen   = (queryString != NULL && queryString[0] != 0) ? (int) strlen(queryString) : 0;

  // base + path + id + (?qs) + NUL
  char* url = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + idLen + 1 + qsLen + 1);
  strcpy(url, base);
  strcpy(url + baseLen, path);
  strcpy(url + baseLen + pathLen, entityId);
  if (qsLen > 0)
  {
    url[baseLen + pathLen + idLen] = '?';
    strcpy(url + baseLen + pathLen + idLen + 1, queryString);
  }

  char* respBody    = NULL;
  int   respBodyLen = 0;

  int status = ldDistOpSendReceive(csr, SwVerbGet, url, NULL, 0, ownAlias,
                                   errorDetailPP, &respBody, &respBodyLen);
  if (status < 200 || status >= 300)
    return status;
  if (respBody == NULL || respBodyLen == 0)
  {
    *errorDetailPP = "empty body in upstream 2xx response";
    return 502;
  }

  KjNode* treeP = kjParse(swRest.kjsonP, respBody);
  if (treeP == NULL)
  {
    *errorDetailPP = "upstream returned malformed JSON";
    return 502;
  }

  swldExpandTree(treeP, swNgsild.contextP, &swRest.kalloc);
  ldStripAtContext(treeP);

  *upstreamPP = treeP;
  return status;
}



bool getEntityTemporal(void)
{
  // § 4.21 / § 6.4.3 — cross-parameter projection validation
  // (pick ∩ omit, pick + attrs, omit + attrs, etc).
  if (ldParamsValidate())
    return true;

  const char* entityId = swRest.in.wildcard[0];

  if (entityId == NULL || entityId[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "missing entity id in URL");
    return true;
  }

  // timerel — optional for the single-entity GET (mandatory only for the
  // multi-entity GET /temporal/entities, per § 6.18.3.2). When present,
  // timeAt is mandatory; when timerel=between, endTimeAt is too.
  if (swNgsild.timerel != NULL)
  {
    if (swNgsild.timeAt == NULL)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "missing required URL parameter 'timeAt' (timerel='%s')", swNgsild.timerel);
      return true;
    }
    if (strcmp(swNgsild.timerel, "between") == 0 && swNgsild.endTimeAt == NULL)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "missing required URL parameter 'endTimeAt' for timerel='between'");
      return true;
    }
  }

  if (troe.entityTemporalRetrieve == NULL)
  {
    ldError(422, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported",
            "Not Implemented",
            "active TRoE plugin does not support temporal queries");
    return true;
  }

  // § 6.3.22 / § 5.5.15 — NGSILD-Snapshot routes the temporal retrieve
  // to the snap-tenant's frozen TRoE store. Distop dispatch is bypassed
  // entirely (the snap-tenant already holds the captured federated view).
  bool                 snapSeen = false;
  LdSnapshotCacheItem* snapItem = ldSnapshotItemFromHeader(&snapSeen);
  if (snapSeen && snapItem == NULL)
    return true;  // 404 already raised by helper

  TroeQueryFilter filter;
  memset(&filter, 0, sizeof(filter));
  filter.timerel      = swNgsild.timerel;
  filter.timeAtIso    = swNgsild.timeAt;
  filter.endTimeAtIso = swNgsild.endTimeAt;
  filter.timeproperty = swNgsild.timeproperty;
  filter.attrV        = swNgsild.attrsV;
  filter.lastN        = swNgsild.lastN;
  filter.datasetIdV   = swNgsild.datasetIdV;

  Tenant* tenantP = (snapItem != NULL)
                      ? (Tenant*) snapItem->snapTenantP
                      : (Tenant*) swNgsild.tenantP;

  TroeRangeInfo rangeInfo;
  memset(&rangeInfo, 0, sizeof(rangeInfo));

  // Distop dispatch: match CSRs that advertise retrieveTemporal. Default
  // operationsMask (federationOps) does NOT include retrieveTemporal per
  // § 4.20 Table 4.20-2 — CSRs must opt in explicitly.
  LdRegCacheItem** exclV  = NULL;
  LdRegCacheItem** redirV = NULL;
  LdRegCacheItem** inclV  = NULL;
  LdRegCacheItem** auxV   = NULL;
  int              exclN  = 0;
  int              redirN = 0;
  int              inclN  = 0;
  int              auxN   = 0;

  if (snapItem == NULL && !swNgsild.local && tenantP != NULL && tenantP->regCacheP != NULL)
  {
    exclN  = ldRegCacheMatchForRetrieve((LdRegCache*) tenantP->regCacheP,
                                        entityId, swNgsild.typeV,
                                        LdRegModeExclusive, &exclV);
    redirN = ldRegCacheMatchForRetrieve((LdRegCache*) tenantP->regCacheP,
                                        entityId, swNgsild.typeV,
                                        LdRegModeRedirect, &redirV);
    inclN  = ldRegCacheMatchForRetrieve((LdRegCache*) tenantP->regCacheP,
                                        entityId, swNgsild.typeV,
                                        LdRegModeInclusive, &inclV);
    auxN   = ldRegCacheMatchForRetrieve((LdRegCache*) tenantP->regCacheP,
                                        entityId, NULL,
                                        LdRegModeAuxiliary, &auxV);
  }

  const char* ownAlias = ldCsourceAliasForTenant(tenantP != NULL ? tenantP->name : NULL,
                                                  &swRest.kalloc);
  if (ldDistOpLoopDetected(ownAlias))
  {
    if (exclV  != NULL) { free(exclV);  exclV  = NULL; exclN  = 0; }
    if (redirV != NULL) { free(redirV); redirV = NULL; redirN = 0; }
    if (inclV  != NULL) { free(inclV);  inclV  = NULL; inclN  = 0; }
    if (auxV   != NULL) { free(auxV);   auxV   = NULL; auxN   = 0; }
  }

  // Local TRoE retrieve. result==NULL is "no temporal data here yet";
  // distops may still produce a response, so we keep going.
  KjNode* result = NULL;
  int     r      = troe.entityTemporalRetrieve(tenantP, entityId, &filter, &result, &rangeInfo);

  if (r != TROE_OK && r != TROE_NOT_FOUND)
  {
    if (exclV  != NULL) free(exclV);
    if (redirV != NULL) free(redirV);
    if (inclV  != NULL) free(inclV);
    if (auxV   != NULL) free(auxV);
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "temporal retrieve failed for entity '%s'", entityId);
    return true;
  }

  bool haveDistop = (exclN > 0 || redirN > 0 || inclN > 0 || auxN > 0);

  if (haveDistop)
  {
    const char* fwdQuery = buildTemporalForwardQuery(&swRest.kalloc);

    // Exclusive (§ 4.3.6.3): strip locally-registered attrs, single source.
    for (int i = 0; i < exclN; i++)
    {
      LdRegCacheItem* csr = exclV[i];
      if (csr->endpoint == NULL || !ldRegOpSupported(csr, LdOpRetrieveTemporal)) continue;
      if (ldDistOpCsrWouldLoop(csr, ownAlias)) continue;
      for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
      {
        if (result != NULL) stripInfoAttrsFromTemporal(result, riP);
        KjNode*     upP   = NULL;
        const char* upErr = NULL;
        int code = forwardTemporalAndParse(csr, entityId, fwdQuery, ownAlias, &upP, &upErr);
        if (code == 404 || upP == NULL) continue;
        if (code < 200 || code >= 300) continue;
        if (result == NULL) result = upP;
        else                mergeTemporalInto(result, upP, false);
      }
    }

    // Redirect (§ 4.3.6.3): strip + forward + merge.
    for (int i = 0; i < redirN; i++)
    {
      LdRegCacheItem* csr = redirV[i];
      if (csr->endpoint == NULL || !ldRegOpSupported(csr, LdOpRetrieveTemporal)) continue;
      if (ldDistOpCsrWouldLoop(csr, ownAlias)) continue;
      for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
      {
        if (result != NULL) stripInfoAttrsFromTemporal(result, riP);
        KjNode*     upP   = NULL;
        const char* upErr = NULL;
        int code = forwardTemporalAndParse(csr, entityId, fwdQuery, ownAlias, &upP, &upErr);
        if (code < 200 || code >= 300 || upP == NULL) continue;
        if (result == NULL) result = upP;
        else                mergeTemporalInto(result, upP, false);
      }
    }

    // Inclusive: forward + merge (no strip).
    for (int i = 0; i < inclN; i++)
    {
      LdRegCacheItem* csr = inclV[i];
      if (csr->endpoint == NULL || !ldRegOpSupported(csr, LdOpRetrieveTemporal)) continue;
      if (ldDistOpCsrWouldLoop(csr, ownAlias)) continue;
      for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
      {
        (void) riP;  // inclusive doesn't need per-RegistrationInfo URL constraints
        KjNode*     upP   = NULL;
        const char* upErr = NULL;
        int code = forwardTemporalAndParse(csr, entityId, fwdQuery, ownAlias, &upP, &upErr);
        if (code < 200 || code >= 300 || upP == NULL) continue;
        if (result == NULL) result = upP;
        else                mergeTemporalInto(result, upP, false);
        break;  // one fetch per CSR is enough for inclusive (the CSR's response covers all its registered attrs)
      }
    }

    // Auxiliary (§ 4.3.6.2): fill gaps only — never overrides.
    for (int i = 0; i < auxN; i++)
    {
      LdRegCacheItem* csr = auxV[i];
      if (csr->endpoint == NULL || !ldRegOpSupported(csr, LdOpRetrieveTemporal)) continue;
      if (ldDistOpCsrWouldLoop(csr, ownAlias)) continue;
      for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
      {
        (void) riP;
        KjNode*     upP   = NULL;
        const char* upErr = NULL;
        int code = forwardTemporalAndParse(csr, entityId, fwdQuery, ownAlias, &upP, &upErr);
        if (code < 200 || code >= 300 || upP == NULL) continue;
        if (result == NULL) result = upP;
        else                mergeTemporalInto(result, upP, true);
        break;
      }
    }
  }

  if (exclV  != NULL) free(exclV);
  if (redirV != NULL) free(redirV);
  if (inclV  != NULL) free(inclV);
  if (auxV   != NULL) free(auxV);

  if (result == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "no temporal data for entity '%s'", entityId);
    return true;
  }

  // § 4.5.4 / § 4.5.5: pick/omit attribute projection (lang reduction
  // and ?format=temporalValues run in renderHook, see ldHooks.c).
  //
  // § 6.3.6: "If the resulting Entity contains no members, the operation
  // shall return ResourceNotFound." Members are id / type / optional
  // scope / user attributes; @context is JSON-LD aux. createdAt /
  // modifiedAt are SystemAttributes and only present with ?sysAttrs=true.
  if (swNgsild.pickV != NULL || swNgsild.omitV != NULL)
  {
    ldPickOmit(result, swNgsild.pickV, swNgsild.omitV);

    bool hasMembers = false;
    for (KjNode* c = result->value.firstChildP; c != NULL; c = c->next)
    {
      if (c->name == NULL)                          continue;
      if (strcmp(c->name, "@context")        == 0)  continue;
      if (!swNgsild.sysAttrs &&
          (strcmp(c->name, LD_VOCAB_CREATED_AT)  == 0 ||
           strcmp(c->name, LD_VOCAB_MODIFIED_AT) == 0 ||
           strcmp(c->name, LD_VOCAB_EXPIRES_AT)  == 0))
        continue;
      hasMembers = true;
      break;
    }
    if (!hasMembers)
    {
      ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
              "entity '%s' has no members after pick/omit projection", entityId);
      return true;
    }
  }

  swRest.out.responseTree = result;

  // § 6.3.10: 206 Partial Content + Content-Range when truncated. Distop
  // results don't currently carry a range; this only fires when the local
  // TRoE result was truncated.
  if (rangeInfo.truncated && rangeInfo.rangeStartIso != NULL && rangeInfo.rangeEndIso != NULL)
  {
    int   sz  = 96;
    char* buf = (char*) kaAlloc(&swRest.kalloc, sz);
    if (rangeInfo.size > 0)
      snprintf(buf, sz, "date-time %s-%s/%d", rangeInfo.rangeStartIso, rangeInfo.rangeEndIso, rangeInfo.size);
    else
      snprintf(buf, sz, "date-time %s-%s/*", rangeInfo.rangeStartIso, rangeInfo.rangeEndIso);
    swRestOutHeaderAdd("Content-Range", buf);
    swRest.out.httpStatusCode = 206;
  }
  else
    swRest.out.httpStatusCode = 200;

  return true;
}

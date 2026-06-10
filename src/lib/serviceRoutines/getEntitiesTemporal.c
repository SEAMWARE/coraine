//
// FILE            getEntitiesTemporal.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/temporal/entities
// NGSI-LD § 5.7.4 — Query Temporal Evolution of Entities (§ 6.18.3.2).
//
// Filtering supported in this slice:
//   ?id (CSV) / ?idPattern / ?type (CSV)        — entity selectors
//   ?timerel + ?timeAt (+ ?endTimeAt for between, mandatory)
//   ?timeproperty                                — observedAt by default
//   ?attrs                                       — attribute name filter
//   ?q                                           — q-tree compiled to SQL
//   ?lastN                                       — per-attr instance cap
//   ?limit / ?offset                             — pagination
//
// Distops (§ 4.3.6 / § 5.7.5): forward to CSRs whose operations[] include
// "queryTemporal". For each remote entity returned, merge into the local
// result by entity ID — same-id entities get their per-attr instance
// arrays concatenated. Multi-source pagination + entity-map are deferred
// (project_temporal_distops_deferred memory).
//

#include <stddef.h>                                  // NULL
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // free
#include <string.h>                                  // strcmp, memset, strlen, strcpy

#include "swRest/SwRestState.h"                      // swRest
#include "swRest/swRestOutHeader.h"                  // swRestOutHeaderAdd
#include "swNgsild/ldPagination.h"                   // ldTemporalPaginationLinkHeader
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjChildAdd, kjChildRemove
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjParse.h"                           // kjParse
#include "kjson/kjClone.h"                           // kjClone
#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "swJsonld/swldExpandTree.h"                 // swldExpandTree

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldParamsValidate.h"               // ldParamsValidate
#include "swNgsild/ldPickOmit.h"                     // ldPickOmit
#include "swNgsild/ldOrderSort.h"                    // ldOrderSort
#include "swNgsild/ldToTemporalValues.h"             // ldToTemporalValues
#include "swNgsild/ldEntityMatch.h"                  // ldEntityMatchScope
#include "swNgsild/ldStripAtContext.h"               // ldStripAtContext
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForQuery, ldRegOpSupported
#include "swNgsild/ldDistOp.h"                       // ldDistOpSendReceive, ldDistOpLoopDetected, ldDistOpCsrWouldLoop
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant

#include "troe/TroeDriver.h"                         // troe, TroeQueryFilter, TroeRangeInfo
#include "troe/troeQTreeToSql.h"                     // troeQTreeToSql
#include "troe/troeNotAvailable.h"                   // troeNotAvailable

#include "db/DbDriver.h"                             // db
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/ldSnapshotRead.h"          // ldSnapshotItemFromHeader
#include "serviceRoutines/getEntitiesTemporal.h"     // Own interface



// -----------------------------------------------------------------------------
//
// stripInfoAttrsFromEntity - drop a RegistrationInfo's covered attrs from one
// EntityTemporal (KjArray-of-instances per attr). Wildcard (no propertyNames /
// relationshipNames) strips all non-keyword children.
//
static void stripInfoAttrsFromEntity(KjNode* entityP, LdRegInfo* riP)
{
  if (entityP == NULL || entityP->type != KjObject)
    return;

  bool wildcard = (riP->propertyNamesV == NULL && riP->relationshipNamesV == NULL);

  KjNode* curP = entityP->value.firstChildP;
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
        kjChildRemove(entityP, curP);
    }

    curP = nextP;
  }
}



// -----------------------------------------------------------------------------
//
// mergeTemporalEntity - merge upstream's per-attr instance arrays into destP.
//
// For temporal entities each non-keyword child is a KjArray of instance
// objects. "Merge" = "for each upstream attr, append its instances onto
// destP's same-named attr (creating it if absent)".
//
// keepOnlyMissing = true → auxiliary mode (§ 4.3.6.2): only copy attrs the
// dest doesn't have yet. Auxiliary registrations fill gaps; they never
// augment data already present.
//
static void mergeTemporalEntity(KjNode* destP, KjNode* upP, bool keepOnlyMissing)
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
      upChild->next = NULL;
      kjChildAdd(destP, upChild);
    }
    else if (!keepOnlyMissing && upChild->type == KjArray && destAttr->type == KjArray)
    {
      KjNode* inst = upChild->value.firstChildP;
      while (inst != NULL)
      {
        KjNode* instNext = inst->next;
        inst->next = NULL;
        kjChildAdd(destAttr, inst);
        inst = instNext;
      }
    }

    upChild = upNext;
  }
}



// -----------------------------------------------------------------------------
//
// buildTemporalQueryQs - rebuild the forwarded query string.
//
// Forwards every URL param except output-shaping ones (pick/omit/lang/format/
// orderBy/options/local/entityMap). Filters that constrain the candidate set
// (id/idPattern/type/q/timerel/timeAt/.../georel/...) all forward verbatim.
//
static const char* buildTemporalQueryQs(KAlloc* kaP)
{
  // Worst-case length: each (key=value&) plus NUL.
  int len = 1;
  for (int i = 0; i < swRest.in.uriParamCount; i++)
  {
    const char* k = swRest.in.uriParamV[i].key;
    if (strcmp(k, "options")   == 0) continue;
    if (strcmp(k, "format")    == 0) continue;
    if (strcmp(k, "local")     == 0) continue;
    if (strcmp(k, "orderBy")   == 0) continue;
    if (strcmp(k, "collation") == 0) continue;
    if (strcmp(k, "entityMap") == 0) continue;
    if (strcmp(k, "pick")      == 0) continue;
    if (strcmp(k, "omit")      == 0) continue;
    if (strcmp(k, "lang")      == 0) continue;
    if (strcmp(k, "scopeQ")    == 0) continue;

    const char* v = swRest.in.uriParamV[i].value;
    len += strlen(k) + 1 + (v ? strlen(v) : 0) + 1;
  }

  char* buf = (char*) kaAlloc(kaP, len);
  int pos = 0;

  for (int i = 0; i < swRest.in.uriParamCount; i++)
  {
    const char* k = swRest.in.uriParamV[i].key;
    if (strcmp(k, "options")   == 0) continue;
    if (strcmp(k, "format")    == 0) continue;
    if (strcmp(k, "local")     == 0) continue;
    if (strcmp(k, "orderBy")   == 0) continue;
    if (strcmp(k, "collation") == 0) continue;
    if (strcmp(k, "entityMap") == 0) continue;
    if (strcmp(k, "pick")      == 0) continue;
    if (strcmp(k, "omit")      == 0) continue;
    if (strcmp(k, "lang")      == 0) continue;
    if (strcmp(k, "scopeQ")    == 0) continue;

    const char* v = swRest.in.uriParamV[i].value;
    if (pos > 0) buf[pos++] = '&';
    int kl = strlen(k);
    memcpy(buf + pos, k, kl);
    pos += kl;
    buf[pos++] = '=';
    if (v != NULL)
    {
      int vl = strlen(v);
      memcpy(buf + pos, v, vl);
      pos += vl;
    }
  }

  buf[pos] = 0;
  return buf;
}



// -----------------------------------------------------------------------------
//
// forwardTemporalQueryToCSR - GET /temporal/entities?<qs> from a CSR.
//
// Returns the parsed KjArray of EntityTemporal trees on success (already
// JSON-LD expanded with @context stripped). Returns NULL on transport or
// parse failure or non-2xx status.
//
static KjNode* forwardTemporalQueryToCSR(LdRegCacheItem* csr,
                                         const char*     queryString,
                                         const char*     ownAlias)
{
  if (csr == NULL || csr->endpoint == NULL)
    return NULL;

  const char* base = csr->endpoint;
  const char* path = "/ngsi-ld/v1/temporal/entities";
  int baseLen = strlen(base);
  int pathLen = strlen(path);
  int qsLen   = (queryString != NULL && queryString[0] != 0) ? (int) strlen(queryString) : 0;

  // base + path + (?qs) + NUL
  char* url = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + 1 + qsLen + 1);
  strcpy(url, base);
  strcpy(url + baseLen, path);
  if (qsLen > 0)
  {
    url[baseLen + pathLen] = '?';
    strcpy(url + baseLen + pathLen + 1, queryString);
  }
  else
    url[baseLen + pathLen] = 0;

  char*       respBody    = NULL;
  int         respBodyLen = 0;
  const char* errDetail   = NULL;

  int status = ldDistOpSendReceive(csr, SwVerbGet, url, NULL, 0, ownAlias,
                                   &errDetail, &respBody, &respBodyLen);
  if (status < 200 || status >= 300 || respBody == NULL || respBodyLen == 0)
    return NULL;

  KjNode* treeP = kjParse(swRest.kjsonP, respBody);
  if (treeP == NULL || treeP->type != KjArray)
    return NULL;

  swldExpandTree(treeP, swNgsild.contextP, &swRest.kalloc);
  ldStripAtContext(treeP);
  return treeP;
}



// -----------------------------------------------------------------------------
//
// findEntityById - find a child entity by string-id within an array.
//
static KjNode* findEntityById(KjNode* arrayP, const char* id)
{
  if (arrayP == NULL || arrayP->type != KjArray || id == NULL)
    return NULL;

  for (KjNode* ep = arrayP->value.firstChildP; ep != NULL; ep = ep->next)
  {
    KjNode* idP = kjLookup(ep, "id");
    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, id) == 0)
      return ep;
  }
  return NULL;
}



// -----------------------------------------------------------------------------
//
// mergeRemoteArray - graft remote entities into local arrayP, merging by ID.
//
// New entities → moved into arrayP. Existing entities → per-attr instance
// arrays concatenated. keepOnlyMissing flag is only honoured for existing
// entities; new entities always get added (auxiliary regs can introduce
// entities the broker never saw).
//
static void mergeRemoteArray(KjNode* arrayP, KjNode* remoteArrayP, bool keepOnlyMissing)
{
  if (arrayP == NULL || remoteArrayP == NULL || remoteArrayP->type != KjArray)
    return;

  KjNode* remEntity = remoteArrayP->value.firstChildP;
  while (remEntity != NULL)
  {
    KjNode* nextRem = remEntity->next;

    KjNode* idP = kjLookup(remEntity, "id");
    if (idP == NULL || idP->type != KjString)
    {
      remEntity = nextRem;
      continue;
    }

    KjNode* localEntity = findEntityById(arrayP, idP->value.s);
    if (localEntity == NULL)
    {
      remEntity->next = NULL;
      kjChildAdd(arrayP, remEntity);
    }
    else
    {
      mergeTemporalEntity(localEntity, remEntity, keepOnlyMissing);
    }

    remEntity = nextRem;
  }
}



// -----------------------------------------------------------------------------
//
// stripInfoAttrsFromArray - apply a RegistrationInfo's coverage to every
// entity in arrayP (used for exclusive/redirect modes).
//
static void stripInfoAttrsFromArray(KjNode* arrayP, LdRegInfo* riP)
{
  if (arrayP == NULL || arrayP->type != KjArray)
    return;
  for (KjNode* ep = arrayP->value.firstChildP; ep != NULL; ep = ep->next)
    stripInfoAttrsFromEntity(ep, riP);
}



bool getEntitiesTemporal(void)
{
  // § 4.21 / § 6.4.3 — cross-parameter projection validation
  // (pick ∩ omit, pick + attrs, omit + attrs, etc).
  if (ldParamsValidate())
    return true;

  // § 6.3.22 / § 5.5.15 — NGSILD-Snapshot routes the temporal query to
  // the snap-tenant's frozen TRoE store. Distop dispatch is bypassed,
  // and snapshot reads count as local for the orderBy-requires-local
  // check below. Parsed early so all subsequent gates can see it.
  bool                 snapSeen = false;
  LdSnapshotCacheItem* snapItem = ldSnapshotItemFromHeader(&snapSeen);
  if (snapSeen && snapItem == NULL)
    return true;

  // § 6.18.3.2: timerel is mandatory on the multi-entity GET (unlike the
  // single-entity retrieve, where it's optional). When present, timeAt
  // is mandatory; for timerel=between, endTimeAt is too.
  if (swNgsild.timerel == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "missing required URL parameter 'timerel'");
    return true;
  }
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

  // § 6.18.3.2: at least one of (id, idPattern, type, attrs, q, georel)
  // must be present. Aligned with /entities (§ 5.7.2.4): local=true is
  // also accepted as a sufficient selector — it scopes the query to the
  // broker's local set, which is itself a hard bound.
  if (swNgsild.idV == NULL && swNgsild.idPattern == NULL && swNgsild.typeV == NULL
      && swNgsild.attrsV == NULL && swNgsild.qExpr == NULL && swNgsild.georel == NULL
      && swNgsild.local == false)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "at least one of 'id', 'idPattern', 'type', 'attrs', 'q', 'georel', or 'local' must be supplied");
    return true;
  }

  // § 5.7.4.4 / § 4.23 — orderBy on multi-entity temporal queries can
  // address any entity member: id, type, scope, an attribute name, an
  // attribute sub-property like "name.createdAt", etc. No restriction
  // applied here — the back end orders the local result and federated
  // sources are best-effort merged in arrival order.

  if (troe.entityTemporalQuery == NULL)
  {
    troeNotAvailable("multi-entity temporal query");
    return true;
  }

  TroeQueryFilter filter;
  memset(&filter, 0, sizeof(filter));
  filter.timerel      = swNgsild.timerel;
  filter.timeAtIso    = swNgsild.timeAt;
  filter.endTimeAtIso = swNgsild.endTimeAt;
  filter.timeproperty = swNgsild.timeproperty;
  filter.attrV        = swNgsild.attrsV;

  // § 11.3.3: the geoquery is checked against the GeoProperty instances
  // within the temporal interval — independent of the ?attrs= selection.
  // When attrs= would exclude the geoproperty, fetch it anyway (the geo
  // filter below needs its instances) and strip it from the response after.
  const char* geoprop          = (swNgsild.geoproperty != NULL) ? swNgsild.geoproperty : "location";
  bool        geopropInjected  = false;

  if (swNgsild.geoRel != NULL && swNgsild.attrsV != NULL)
  {
    int n = 0;
    while (swNgsild.attrsV[n] != NULL)
    {
      if (strcmp(swNgsild.attrsV[n], geoprop) == 0)
        break;
      n++;
    }

    if (swNgsild.attrsV[n] == NULL)  // geoprop not in attrs= — inject it
    {
      char** augmented = (char**) kaAlloc(&swRest.kalloc, (n + 2) * sizeof(char*));
      memcpy(augmented, swNgsild.attrsV, n * sizeof(char*));
      augmented[n]     = (char*) geoprop;
      augmented[n + 1] = NULL;
      filter.attrV     = augmented;
      geopropInjected  = true;
    }
  }

  filter.lastN        = swNgsild.lastN;
  filter.firstN       = swNgsild.firstN;
  filter.offsetN      = swNgsild.offsetN;
  filter.datasetIdV   = swNgsild.datasetIdV;
  filter.idV          = swNgsild.idV;
  filter.idPattern    = swNgsild.idPattern;
  filter.typeV        = swNgsild.typeV;
  filter.limit        = swNgsild.limit;
  filter.offset       = swNgsild.offset;

  if (swNgsild.qExpr != NULL)
    filter.qSqlPredicate = troeQTreeToSql(swNgsild.qExpr, &swRest.kalloc);

  Tenant* tenantP = (snapItem != NULL)
                      ? (Tenant*) snapItem->snapTenantP
                      : (Tenant*) swNgsild.tenantP;

  TroeRangeInfo rangeInfo;
  memset(&rangeInfo, 0, sizeof(rangeInfo));

  KjNode* result = NULL;
  int     r      = troe.entityTemporalQuery(tenantP, &filter, &result, &rangeInfo);

  if (r != TROE_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "temporal query failed");
    return true;
  }

  // No matches yet → start with empty array; distops may still contribute.
  if (result == NULL)
    result = kjArray(swRest.kjsonP, NULL);

  // Distop dispatch (§ 4.3.6 / § 5.7.5). queryTemporal is NOT in the
  // default operations group per § 4.20 Table 4.20-2 — CSRs must opt in
  // explicitly. First slice: no-split federation only (no entityMap, no
  // multi-source pagination). All filters are forwarded; broker applies
  // pick/omit/orderBy/scopeQ/geoQ on the merged result.
  if (snapItem == NULL && !swNgsild.local && tenantP != NULL && tenantP->regCacheP != NULL)
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

    if (!ldDistOpLoopDetected(ownAlias))
    {
      LdRegMode   modes[] = { LdRegModeExclusive, LdRegModeRedirect, LdRegModeInclusive, LdRegModeAuxiliary };
      const char* fwdQs   = buildTemporalQueryQs(&swRest.kalloc);

      LdRegCacheItem** matchV[4] = { NULL, NULL, NULL, NULL };
      int              matchN[4] = { 0, 0, 0, 0 };
      int              total     = 0;
      for (int m = 0; m < 4; m++)
      {
        LdRegMode    mode        = modes[m];
        const char** typeFilterV = (mode == LdRegModeAuxiliary) ? NULL : (const char**) swNgsild.typeV;
        matchN[m] = ldRegCacheMatchForQuery((LdRegCache*) tenantP->regCacheP,
                                            swNgsild.idV, swNgsild.idPattern,
                                            (char**) typeFilterV, mode, &matchV[m]);
        total += matchN[m];
      }

      // Pre-strip locals for excl/redir CSRs (§ 4.3.6.3) — independent of
      // forward outcome, so do it up-front in the original sequence.
      for (int m = 0; m < 2; m++)  // exclusive (0), redirect (1)
      {
        for (int i = 0; i < matchN[m]; i++)
        {
          LdRegCacheItem* csr = matchV[m][i];
          if (csr->endpoint == NULL) continue;
          if (!ldRegOpSupported(csr, LdOpQueryTemporal)) continue;
          if (ldDistOpCsrWouldLoop(csr, ownAlias)) continue;
          for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
            stripInfoAttrsFromArray(result, riP);
        }
      }

      LdDistOpBatchItem*   items     = (LdDistOpBatchItem*)   kaAlloc(&swRest.kalloc, total * sizeof(LdDistOpBatchItem));
      memset(items, 0, total * sizeof(LdDistOpBatchItem));
      LdDistOpBatchResult* results   = (LdDistOpBatchResult*) kaAlloc(&swRest.kalloc, total * sizeof(LdDistOpBatchResult));
      int*                 itemMode  = (int*)                 kaAlloc(&swRest.kalloc, total * sizeof(int));
      int                  itemCount = 0;
      memset(results, 0, total * sizeof(LdDistOpBatchResult));

      const char* tpath = "/ngsi-ld/v1/temporal/entities";
      int qsLen = (fwdQs != NULL && fwdQs[0] != 0) ? (int) strlen(fwdQs) : 0;
      int pathLen = strlen(tpath);

      for (int m = 0; m < 4; m++)
      {
        for (int i = 0; i < matchN[m]; i++)
        {
          LdRegCacheItem* csr = matchV[m][i];
          if (csr->endpoint == NULL) continue;
          if (!ldRegOpSupported(csr, LdOpQueryTemporal)) continue;
          if (ldDistOpCsrWouldLoop(csr, ownAlias)) continue;

          int baseLen = strlen(csr->endpoint);
          char* url = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + 1 + qsLen + 1);
          strcpy(url, csr->endpoint);
          strcpy(url + baseLen, tpath);
          if (qsLen > 0)
          {
            url[baseLen + pathLen] = '?';
            strcpy(url + baseLen + pathLen + 1, fwdQs);
          }
          else
            url[baseLen + pathLen] = 0;

          items[itemCount].csr     = csr;
          items[itemCount].url     = url;
          items[itemCount].body    = NULL;
          items[itemCount].bodyLen = 0;
          itemMode[itemCount]      = m;
          itemCount++;
        }
      }

      if (itemCount > 0)
      {
        ldDistOpSendMulti(items, itemCount, SwVerbGet, ownAlias, results);

        for (int i = 0; i < itemCount; i++)
        {
          int upCode = results[i].statusCode;
          if (upCode < 200 || upCode >= 300) continue;
          if (results[i].responseBody == NULL || results[i].responseBodyLen == 0) continue;

          KjNode* remoteArray = kjParse(swRest.kjsonP, results[i].responseBody);
          if (remoteArray == NULL || remoteArray->type != KjArray) continue;

          swldExpandTree(remoteArray, swNgsild.contextP, &swRest.kalloc);
          ldStripAtContext(remoteArray);

          bool keepOnlyMissing = (modes[itemMode[i]] == LdRegModeAuxiliary);
          mergeRemoteArray(result, remoteArray, keepOnlyMissing);
        }
      }

      for (int m = 0; m < 4; m++)
        ldRegCacheMatchRelease(matchV[m], matchN[m]);
    }
  }

  // § 11.3.3 (TS 104-175): the geoquery's restrictions "shall be checked
  // against the GeoProperty instances that are within the interval defined
  // by the temporal query" — i.e. against the instances in the temporal
  // result itself, never against the current state (an entity may exist
  // only in TRoE). ANY matching instance keeps the entity. Each instance's
  // geometry is wrapped as a minimal DB-model entity for the plugin's geo
  // matcher.
  if (swNgsild.geoRel != NULL && db.geoMatchFunc != NULL)
  {
    KjNode* ep = result->value.firstChildP;
    while (ep != NULL)
    {
      KjNode* nextEp = ep->next;
      KjNode* attrP  = kjLookup(ep, geoprop);
      bool    keep   = false;

      if (attrP != NULL && attrP->type == KjArray)
      {
        for (KjNode* instP = attrP->value.firstChildP; instP != NULL; instP = instP->next)
        {
          if (instP->type != KjObject)
            continue;

          KjNode* valueP = kjLookup(instP, "value");
          if (valueP == NULL)
            continue;

          // Minimal DB-model wrapper: { <geoprop>: { "@none": { "value": <geom> } } }
          KjNode* geomP   = kjClone(swRest.kjsonP, valueP);
          KjNode* dsInstP = kjObject(swRest.kjsonP, "@none");
          KjNode* wrapP   = kjObject(swRest.kjsonP, geoprop);
          KjNode* synthP  = kjObject(swRest.kjsonP, NULL);

          geomP->name = (char*) "value";
          kjChildAdd(dsInstP, geomP);
          kjChildAdd(wrapP, dsInstP);
          kjChildAdd(synthP, wrapP);

          if (db.geoMatchFunc(synthP, swNgsild.geoRel, swNgsild.geometry, swNgsild.coordinates, geoprop))
          {
            keep = true;
            break;
          }
        }
      }

      if (!keep)
        kjChildRemove(result, ep);

      ep = nextEp;
    }

    // The geoproperty was fetched only for the geo filter — ?attrs= does
    // not select it, so strip it from the surviving entities.
    if (geopropInjected)
    {
      for (KjNode* ep2 = result->value.firstChildP; ep2 != NULL; ep2 = ep2->next)
      {
        KjNode* gP = kjLookup(ep2, geoprop);
        if (gP != NULL)
          kjChildRemove(ep2, gP);
      }
    }
  }

  // § 4.18 / § 6.18.3.2: scopeQ — applied against the entity's CURRENT
  // state (looked up from the current-state DB). Temporal entities that no
  // longer exist in current state can't satisfy a current-state filter and
  // are dropped.
  if (swNgsild.scopeExpr != NULL && db.entityRetrieve != NULL)
  {
    KjNode* ep = result->value.firstChildP;
    while (ep != NULL)
    {
      KjNode* nextEp = ep->next;
      KjNode* idP    = kjLookup(ep, "id");
      bool    keep   = false;

      KjNode* curEntity = NULL;
      if (idP != NULL && idP->type == KjString)
        db.entityRetrieve(tenantP, idP->value.s, &curEntity);

      if (curEntity != NULL)
      {
        KjNode* scopeP = kjLookup(curEntity, "scope");
        keep = ldEntityMatchScope(scopeP, swNgsild.scopeExpr);
      }

      if (!keep)
        kjChildRemove(result, ep);

      ep = nextEp;
    }
  }

  // § 4.23: orderBy. For temporal output the sorter picks the most-recent
  // instance value per entity (see ldOrderSort.c::temporalLatestValue).
  if (swNgsild.orderByV != NULL && swNgsild.orderByCount > 0)
    ldOrderSort(result, swNgsild.orderByV, swNgsild.orderByCount);

  // § 4.5.4 / § 4.5.5: pick/omit attribute projection (lang reduction
  // and ?format=temporalValues run in renderHook, see ldHooks.c).
  if (swNgsild.pickV != NULL || swNgsild.omitV != NULL)
  {
    for (KjNode* ep = result->value.firstChildP; ep != NULL; ep = ep->next)
      ldPickOmit(ep, swNgsild.pickV, swNgsild.omitV);
  }

  swRest.out.responseTree = result;

  // § 6.4.7.3: when any entity's instances remain beyond the returned
  // page, emit Link rel="intervalafter"/"intervalbefore" page pointers.
  ldTemporalPaginationLinkHeader(rangeInfo.hasMore, rangeInfo.size);
  swRest.out.httpStatusCode = 200;

  return true;
}

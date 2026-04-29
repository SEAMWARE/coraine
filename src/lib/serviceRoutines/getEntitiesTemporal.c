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
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjChildAdd, kjChildRemove
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjParse.h"                           // kjParse
#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "swJsonld/swldExpandTree.h"                 // swldExpandTree

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldPickOmit.h"                     // ldPickOmit
#include "swNgsild/ldOrderSort.h"                    // ldOrderSort
#include "swNgsild/ldToTemporalValues.h"             // ldToTemporalValues
#include "swNgsild/ldEntityMatch.h"                  // ldEntityMatchScope
#include "swNgsild/ldStripAtContext.h"               // ldStripAtContext
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieve, ldRegOpSupported
#include "swNgsild/ldDistOp.h"                       // ldDistOpSendReceive, ldDistOpLoopDetected, ldDistOpCsrWouldLoop
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant

#include "troe/TroeDriver.h"                         // troe, TroeQueryFilter, TroeRangeInfo
#include "troe/troeQTreeToSql.h"                     // troeQTreeToSql

#include "db/DbDriver.h"                             // db
#include "db/Tenant.h"                               // Tenant

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

  swldExpandTree(treeP, &swRest.kalloc);
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
  // must be present. attrs is also a synonym for pick+q in this route's
  // table — the deprecated combined form. For this slice we just enforce
  // the simple "one of" rule.
  if (swNgsild.idV == NULL && swNgsild.idPattern == NULL && swNgsild.typeV == NULL
      && swNgsild.attrsV == NULL && swNgsild.qExpr == NULL && swNgsild.georel == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "at least one of 'id', 'idPattern', 'type', 'attrs', 'q', 'georel' must be supplied");
    return true;
  }

  if (troe.entityTemporalQuery == NULL)
  {
    ldError(422, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported",
            "Not Implemented",
            "active TRoE plugin does not support multi-entity temporal queries");
    return true;
  }

  TroeQueryFilter filter;
  memset(&filter, 0, sizeof(filter));
  filter.timerel      = swNgsild.timerel;
  filter.timeAtIso    = swNgsild.timeAt;
  filter.endTimeAtIso = swNgsild.endTimeAt;
  filter.timeproperty = swNgsild.timeproperty;
  filter.attrV        = swNgsild.attrsV;
  filter.lastN        = swNgsild.lastN;
  filter.datasetIdV   = swNgsild.datasetIdV;
  filter.idV          = swNgsild.idV;
  filter.idPattern    = swNgsild.idPattern;
  filter.typeV        = swNgsild.typeV;
  filter.limit        = swNgsild.limit;
  filter.offset       = swNgsild.offset;

  if (swNgsild.qExpr != NULL)
    filter.qSqlPredicate = troeQTreeToSql(swNgsild.qExpr, &swRest.kalloc);

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

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
  if (!swNgsild.local && tenantP != NULL && tenantP->regCacheP != NULL)
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

    if (!ldDistOpLoopDetected(ownAlias))
    {
      LdRegMode   modes[] = { LdRegModeExclusive, LdRegModeRedirect, LdRegModeInclusive, LdRegModeAuxiliary };
      const char* fwdQs   = buildTemporalQueryQs(&swRest.kalloc);

      for (int m = 0; m < 4; m++)
      {
        LdRegMode         mode    = modes[m];
        bool              keepOnlyMissing = (mode == LdRegModeAuxiliary);
        LdRegCacheItem**  matchV  = NULL;
        int               matchN  = 0;

        // Auxiliary ignores typeV — it fills gaps regardless of the
        // user-supplied type filter (matching getEntities's auxiliary call).
        const char** typeFilterV = (mode == LdRegModeAuxiliary) ? NULL : (const char**) swNgsild.typeV;

        matchN = ldRegCacheMatchForRetrieve((LdRegCache*) tenantP->regCacheP,
                                            NULL, (char**) typeFilterV,
                                            mode, &matchV);

        for (int i = 0; i < matchN; i++)
        {
          LdRegCacheItem* csr = matchV[i];
          if (csr->endpoint == NULL) continue;
          if (!ldRegOpSupported(csr, LdOpQueryTemporal)) continue;
          if (ldDistOpCsrWouldLoop(csr, ownAlias)) continue;

          // Exclusive / redirect (§ 4.3.6.3): registered attrs must be
          // dropped from the local result before the remote replaces them.
          // Inclusive / auxiliary keep the local attrs intact.
          if (mode == LdRegModeExclusive || mode == LdRegModeRedirect)
          {
            for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
              stripInfoAttrsFromArray(result, riP);
          }

          KjNode* remoteArray = forwardTemporalQueryToCSR(csr, fwdQs, ownAlias);
          if (remoteArray == NULL)
            continue;

          mergeRemoteArray(result, remoteArray, keepOnlyMissing);
        }

        if (matchV != NULL)
          free(matchV);
      }
    }
  }

  // § 4.18 / § 6.18.3.2: scopeQ and geoQ — both applied against the
  // entity's CURRENT state (looked up from the current-state DB). Temporal
  // entities that no longer exist in current state can't satisfy a
  // current-state filter and are dropped. Same single DB-fetch covers both.
  bool needCurrentState = (swNgsild.scopeExpr != NULL || swNgsild.geoRel != NULL);

  if (needCurrentState && db.entityRetrieve != NULL)
  {
    KjNode* ep = result->value.firstChildP;
    while (ep != NULL)
    {
      KjNode* nextEp = ep->next;
      KjNode* idP    = kjLookup(ep, "id");
      bool    keep   = true;

      KjNode* curEntity = NULL;
      if (idP != NULL && idP->type == KjString)
        db.entityRetrieve(tenantP, idP->value.s, &curEntity);

      if (curEntity == NULL)
      {
        keep = false;
      }
      else
      {
        if (keep && swNgsild.scopeExpr != NULL)
        {
          KjNode* scopeP = kjLookup(curEntity, "scope");
          keep = ldEntityMatchScope(scopeP, swNgsild.scopeExpr);
        }

        if (keep && swNgsild.geoRel != NULL && db.geoMatchFunc != NULL)
        {
          keep = db.geoMatchFunc(curEntity, swNgsild.geoRel, swNgsild.geometry,
                                 swNgsild.coordinates,
                                 swNgsild.geoproperty ? swNgsild.geoproperty : "location");
        }
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

  // § 6.3.10: 206 Partial Content + Content-Range when any entity was truncated.
  // The bounds span the union of all entities' attribute time ranges in the
  // response — there's only one Content-Range header per HTTP response.
  if (rangeInfo.truncated && rangeInfo.rangeStartIso != NULL && rangeInfo.rangeEndIso != NULL)
  {
    int   sz  = 96;
    char* buf = (char*) kaAlloc(&swRest.kalloc, sz);
    if (rangeInfo.size > 0)
      snprintf(buf, sz, "DateTime %s-%s/%d", rangeInfo.rangeStartIso, rangeInfo.rangeEndIso, rangeInfo.size);
    else
      snprintf(buf, sz, "DateTime %s-%s/*", rangeInfo.rangeStartIso, rangeInfo.rangeEndIso);
    swRestOutHeaderAdd("Content-Range", buf);
    swRest.out.httpStatusCode = 206;
  }
  else
    swRest.out.httpStatusCode = 200;

  return true;
}

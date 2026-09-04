//
// FILE            getEntitiesTemporal.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
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

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/corRestOutHeader.h"                  // corRestOutHeaderAdd
#include "corRest/corRestUrlValueEncode.h"             // corRestUrlValueEncode
#include "corNgsild/ldPagination.h"                   // ldTemporalPaginationLinkHeader
#include "corNgsild/ldToAggregatedValues.h"           // ldAggrMethodValid
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjChildAdd, kjChildRemove
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjParse.h"                           // kjParse
#include "kjson/kjClone.h"                           // kjClone
#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "corJsonld/corLdExpandTree.h"                 // corLdExpandTree

#include "corNgsild/LdQ.h"                            // LdQNode
#include "corNgsild/LdProj.h"                          // LdProjItem
#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdGeoRel.h"                        // LdGeoNear
#include "corNgsild/ldParams.h"                        // LD_PARAM_LIMIT
#include "corNgsild/ldParamsValidate.h"               // ldParamsValidate
#include "corNgsild/ldPickOmit.h"                     // ldPickOmit
#include "corNgsild/ldOrderSort.h"                    // ldOrderSort
#include "corNgsild/ldToTemporalValues.h"             // ldToTemporalValues
#include "corNgsild/ldEntityMatch.h"                  // ldEntityMatchScope
#include "corNgsild/ldStripAtContext.h"               // ldStripAtContext
#include "corNgsild/ldRegCache.h"                     // ldRegCacheMatchForQuery, ldRegOpSupported
#include "corNgsild/ldDistOp.h"                       // ldDistOpSendReceive, ldDistOpLoopDetected, ldDistOpCsrWouldLoop
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant

#include "troe/TroeDriver.h"                         // troe, TroeQueryFilter, TroeRangeInfo
#include "troe/troeQTreeToSql.h"                     // troeQTreeToSql
#include "troe/troeNotAvailable.h"                   // troeNotAvailable

#include "db/DbDriver.h"                             // db
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/ldSnapshotRead.h"          // ldSnapshotItemFromHeader
#include "serviceRoutines/getEntitiesTemporal.h"     // Own interface



// -----------------------------------------------------------------------------
//
// csfCsrMatch - § 5.2.23 Context Source Filter: may this registration serve the
// query? The filter addresses the registration's own Properties, never the
// Entities it holds, so a registration we cannot inspect cannot satisfy it.
//
static bool csfCsrMatch(LdRegCacheItem* csr)
{
  if (corNgsild.csfExpr == NULL)
    return true;

  return (csr->regTree != NULL) && ldEntityMatchQ(csr->regTree, corNgsild.csfExpr);
}



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

  bool wildcard = (riP->attributeNamesV == NULL);

  KjNode* curP = entityP->value.firstChildP;
  while (curP != NULL)
  {
    KjNode* nextP = curP->next;

    if (curP->name != NULL && curP->name[0] != '@' &&
        strcmp(curP->name, "id")   != 0 &&
        strcmp(curP->name, "type") != 0)
    {
      bool covered = wildcard;
      if (!covered && riP->attributeNamesV != NULL)
        for (int j = 0; riP->attributeNamesV[j] != NULL; j++)
          if (strcmp(curP->name, riP->attributeNamesV[j]) == 0) { covered = true; break; }

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
  // Worst-case length: each (key=value&) plus NUL, plus "&sysAttrs=true".
  int len = 1 + 15;
  for (int i = 0; i < corRest.in.uriParamCount; i++)
  {
    const char* k = corRest.in.uriParamV[i].key;
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
    if (strcmp(k, "sysAttrs")  == 0) continue;  // re-emitted below from the parsed flag

    // 3x the value: a percent-encoded byte becomes three
    const char* v = corRest.in.uriParamV[i].value;
    len += strlen(k) + 1 + (v ? 3 * strlen(v) : 0) + 1;
  }

  char* buf = (char*) kaAlloc(kaP, len);
  int pos = 0;

  for (int i = 0; i < corRest.in.uriParamCount; i++)
  {
    const char* k = corRest.in.uriParamV[i].key;
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
    if (strcmp(k, "sysAttrs")  == 0) continue;  // re-emitted below from the parsed flag

    //
    // Encoded, not verbatim: the value arrived percent-DECODED and a raw '&'
    // would start a new parameter. timeAt is the one that bites here — an ISO
    // 8601 offset is spelled `+01:00`, and a raw '+' reaches the source as a
    // space, which is not a DateTime at all.
    //
    const char* v = corRestUrlValueEncode(corRest.in.uriParamV[i].value, kaP);
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

  //
  // sysAttrs. A temporal response assembles per-instance time series rather
  // than resolving § 4.5.5.3 conflicts, so the sources are asked for System
  // Attributes only when the CLIENT wants them - but then they must actually
  // be asked. Emitted from the parsed flag because the client has two
  // spellings (`sysAttrs=true`, `options=sysAttrs`) and `options` is not
  // forwarded, so the raw route honoured one and silently dropped the other.
  //
  if (corNgsild.sysAttrs)
  {
    if (pos > 0) buf[pos++] = '&';
    memcpy(buf + pos, "sysAttrs=true", 13);
    pos += 13;
  }

  buf[pos] = 0;
  return buf;
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
// entityInfoCoversId - does any EntityInfo entry in riP cover entityId?
//
static bool entityInfoCoversId(LdRegInfo* riP, const char* entityId)
{
  for (LdRegEntityInfo* eiP = riP->entityInfoV; eiP != NULL; eiP = eiP->next)
  {
    if (eiP->id == NULL && eiP->idPatternList == NULL)
      return true;
    if (eiP->id != NULL && entityId != NULL && strcmp(eiP->id, entityId) == 0)
      return true;
    for (LdRegIdPattern* patP = eiP->idPatternList; patP != NULL; patP = patP->next)
      if (entityId != NULL && regexec(&patP->regex, entityId, 0, NULL, 0) == 0)
        return true;
  }
  return false;
}



// -----------------------------------------------------------------------------
//
// stripInfoAttrsFromArray - apply a RegistrationInfo's coverage to the entities
// in arrayP it actually covers (used for exclusive/redirect modes).
//
// The attribute names are only half of the claim - the RegistrationInfo's
// entityInfo[] says WHICH entities they are claimed for. Stripping by name
// alone would hide a locally-owned attribute of an entity the registration
// never mentioned: the claim is per (entity, attribute) pair, and § 9.3.3
// keeps exclusive entries pinned to a specific id precisely so that pair is
// unambiguous.
//
static void stripInfoAttrsFromArray(KjNode* arrayP, LdRegInfo* riP)
{
  if (arrayP == NULL || arrayP->type != KjArray)
    return;

  for (KjNode* ep = arrayP->value.firstChildP; ep != NULL; ep = ep->next)
  {
    KjNode* idP = kjLookup(ep, "id");
    if ((idP != NULL) && (idP->type == KjString) && !entityInfoCoversId(riP, idP->value.s))
      continue;

    stripInfoAttrsFromEntity(ep, riP);
  }
}



bool getEntitiesTemporal(void)
{
  // § 4.21 / § 6.4.3 — cross-parameter projection validation
  // (pick ∩ omit, pick + attrs, omit + attrs, etc).
  if (ldParamsValidate())
    return true;

  //
  // § 11.3.3.4: "If projection attributes or filter conditions indicate the use
  // of Linked Entity retrieval, an error of type BadRequestData shall be
  // raised." Same reason as the single-entity temporal retrieval: a `{...}`
  // sub-projection used to be accepted and then quietly dropped.
  //
  for (LdProjItem* pP = corNgsild.pickTree; pP != NULL; pP = pP->next)
  {
    if (pP->child != NULL)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Projection",
              "'pick' uses a linked-entity projection ('%s'), which a temporal query does not support",
              (corNgsild.pick != NULL) ? corNgsild.pick : "");
      return true;
    }
  }

  for (LdProjItem* pP = corNgsild.omitTree; pP != NULL; pP = pP->next)
  {
    if (pP->child != NULL)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Projection",
              "'omit' uses a linked-entity projection ('%s'), which a temporal query does not support",
              (corNgsild.omit != NULL) ? corNgsild.omit : "");
      return true;
    }
  }

  //
  // § 11.3.3.4 continues: "...or filter conditions indicate the use of Linked
  // Entity retrieval, an error of type BadRequestData shall be raised."
  //
  // The q counterpart of the brace projection is the § 4.9 LinkedEntityRelation,
  // `q=owner{age>30}`, which ldQParse records as a chain depth on the root. The
  // temporal query never followed those relationships, so the condition was
  // simply dropped and the entity came back anyway: `q=owner{age>50}` against a
  // 35-year-old owner still returned the vehicle. An ignored filter yields wrong
  // ENTITIES, not merely extra fields, so this is refused rather than tolerated.
  //
  if ((corNgsild.qExpr != NULL) && (corNgsild.qExpr->linkedDepth > 0))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Query",
            "'q' uses a linked-entity relation ('%s'), which a temporal query does not support",
            (corNgsild.q != NULL) ? corNgsild.q : "");
    return true;
  }

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
  if (corNgsild.timerel == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Missing URL Parameter",
            "missing required URL parameter 'timerel'");
    return true;
  }
  if (corNgsild.timeAt == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Missing URL Parameter",
            "missing required URL parameter 'timeAt' (timerel='%s')", corNgsild.timerel);
    return true;
  }
  if (strcmp(corNgsild.timerel, "between") == 0 && corNgsild.endTimeAt == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Missing URL Parameter",
            "missing required URL parameter 'endTimeAt' for timerel='between'");
    return true;
  }

  // § 5.2.6.7.4: aggrMethods cardinality is 1 when aggregatedValues is the
  // requested representation — i.e. it is mandatory. Without it there is
  // nothing to aggregate, so the request is malformed.
  if (corNgsild.format == LdFormatAggregatedValues && corNgsild.aggrMethodsV == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Missing URL Parameter",
            "'aggrMethods' is required when the requested format is 'aggregatedValues'");
    return true;
  }

  // § 3.2.7: aggrMethods is a closed enum. An unrecognised method would be
  // silently dropped (empty aggregation), so reject it with 400 BadRequestData.
  if (corNgsild.format == LdFormatAggregatedValues)
  {
    for (int i = 0; corNgsild.aggrMethodsV[i] != NULL; i++)
    {
      if (!ldAggrMethodValid(corNgsild.aggrMethodsV[i]))
      {
        ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid URL Parameter",
                "'%s' is not a valid aggregation method (§ 3.2.7)", corNgsild.aggrMethodsV[i]);
        return true;
      }
    }
  }

  // § 6.18.3.2: at least one of (id, idPattern, type, attrs, q, georel)
  // must be present. Aligned with /entities (§ 5.7.2.4): local=true is
  // also accepted as a sufficient selector — it scopes the query to the
  // broker's local set, which is itself a hard bound.
  if (corNgsild.idV == NULL && corNgsild.idPattern == NULL && corNgsild.typeV == NULL
      && corNgsild.attrsV == NULL && corNgsild.qExpr == NULL && corNgsild.georel == NULL
      && corNgsild.local == false)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Query Too Broad",
            "at least one of 'id', 'idPattern', 'type', 'attrs', 'q', 'georel', or 'local' must be supplied");
    return true;
  }

  // § 6.4.6: an explicit limit=0 is permitted ONLY together with ?count (a
  // count-only request). The present-params bitmask distinguishes an explicit
  // 0 from an absent limit (absent → broker default page size).
  bool limitGiven = (corRest.in.uriParamMask & LD_PARAM_LIMIT) != 0;
  if (limitGiven && corNgsild.limit == 0 && corNgsild.count == false)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid URL Parameter",
            "'limit' of 0 is only allowed together with 'count'");
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
  filter.timerel      = corNgsild.timerel;
  filter.timeAtIso    = corNgsild.timeAt;
  filter.endTimeAtIso = corNgsild.endTimeAt;
  filter.timeproperty = corNgsild.timeproperty;
  filter.attrV        = corNgsild.attrsV;

  // § 11.3.3 geoquery pushdown: every georel is resolved in SQL by the
  // timescale plugin (PostGIS) — checked against the GeoProperty instances
  // that fall within the temporal window, independent of ?attrs=. The plugin
  // restricts the result to matching entities, so the broker neither injects
  // the geoproperty into ?attrs= nor re-filters afterwards. (Distributed
  // sources apply the forwarded georel themselves.)
  const char* geoprop = (corNgsild.geoproperty != NULL) ? corNgsild.geoproperty : "location";

  if (corNgsild.geoRel != NULL)
  {
    filter.geoRelType     = corNgsild.geoRel->rel;
    filter.geoMaxDistance = corNgsild.geoRel->maxDistance;
    filter.geoMinDistance = corNgsild.geoRel->minDistance;
    filter.geoGeometry    = corNgsild.geometry;
    filter.geoCoordinates = corNgsild.coordinates;
    filter.geoProperty    = geoprop;
  }

  filter.lastN        = corNgsild.lastN;
  filter.firstN       = corNgsild.firstN;
  filter.offsetN      = corNgsild.offsetN;
  filter.datasetIdV   = corNgsild.datasetIdV;
  filter.idV          = corNgsild.idV;
  filter.idPattern    = corNgsild.idPattern;
  filter.typeV        = corNgsild.typeV;
  filter.limit        = corNgsild.limit;
  filter.offset       = corNgsild.offset;
  filter.count        = corNgsild.count;
  filter.limitGiven   = limitGiven;

  if (corNgsild.qExpr != NULL)
  {
    filter.qSqlPredicate = troeQTreeToSql(corNgsild.qExpr, &corRest.kalloc);

    //
    // A q that cannot be turned into SQL must not simply be left out: the
    // query would then run unfiltered and answer with Entities that do not
    // match what was asked for. Silently wrong data is worse than an error.
    //
    if (filter.qSqlPredicate == NULL)
    {
      ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Operation Not Supported",
              "this 'q' cannot be evaluated against the temporal store");
      return true;
    }
  }

  Tenant* tenantP = (snapItem != NULL)
                      ? (Tenant*) snapItem->snapTenantP
                      : (Tenant*) corNgsild.tenantP;

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
    result = kjArray(corRest.kjsonP, NULL);

  // Distop dispatch (§ 4.3.6 / § 5.7.5). queryTemporal is NOT in the
  // default operations group per § 4.20 Table 4.20-2 — CSRs must opt in
  // explicitly. First slice: no-split federation only (no entityMap, no
  // multi-source pagination). All filters are forwarded; broker applies
  // pick/omit/orderBy/scopeQ/geoQ on the merged result.
  if (snapItem == NULL && !corNgsild.local && tenantP != NULL && tenantP->regCacheP != NULL)
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);

    if (!ldDistOpLoopDetected(ownAlias))
    {
      LdRegMode   modes[] = { LdRegModeExclusive, LdRegModeRedirect, LdRegModeInclusive, LdRegModeAuxiliary };
      const char* fwdQs   = buildTemporalQueryQs(&corRest.kalloc);

      LdRegCacheItem** matchV[4] = { NULL, NULL, NULL, NULL };
      int              matchN[4] = { 0, 0, 0, 0 };
      int              total     = 0;
      for (int m = 0; m < 4; m++)
      {
        LdRegMode    mode        = modes[m];
        const char** typeFilterV = (mode == LdRegModeAuxiliary) ? NULL : (const char**) corNgsild.typeV;
        matchN[m] = ldRegCacheMatchForQuery((LdRegCache*) tenantP->regCacheP,
                                            corNgsild.idV, corNgsild.idPattern,
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
          if (!csfCsrMatch(csr)) continue;
          for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
            stripInfoAttrsFromArray(result, riP);
        }
      }

      LdDistOpBatchItem*   items     = (LdDistOpBatchItem*)   kaAlloc(&corRest.kalloc, total * sizeof(LdDistOpBatchItem));
      memset(items, 0, total * sizeof(LdDistOpBatchItem));
      LdDistOpBatchResult* results   = (LdDistOpBatchResult*) kaAlloc(&corRest.kalloc, total * sizeof(LdDistOpBatchResult));
      int*                 itemMode  = (int*)                 kaAlloc(&corRest.kalloc, total * sizeof(int));
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
          if (!csfCsrMatch(csr)) continue;

          int baseLen = strlen(csr->endpoint);
          char* url = (char*) kaAlloc(&corRest.kalloc, baseLen + pathLen + 1 + qsLen + 1);
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
        ldDistOpSendMulti(items, itemCount, CorVerbGet, ownAlias, results);

        for (int i = 0; i < itemCount; i++)
        {
          int upCode = results[i].statusCode;
          if (upCode < 200 || upCode >= 300) continue;
          if (results[i].responseBody == NULL || results[i].responseBodyLen == 0) continue;

          KjNode* remoteArray = results[i].responseTree;
          if (remoteArray == NULL || remoteArray->type != KjArray) continue;

          corLdExpandTree(remoteArray, corNgsild.contextP, &corRest.kalloc);
          ldStripAtContext(remoteArray);

          bool keepOnlyMissing = (modes[itemMode[i]] == LdRegModeAuxiliary);
          mergeRemoteArray(result, remoteArray, keepOnlyMissing);
        }
      }

      for (int m = 0; m < 4; m++)
        ldRegCacheMatchRelease(matchV[m], matchN[m]);
    }
  }

  // § 11.3.3 geoquery: resolved in SQL by the timescale plugin (PostGIS) for
  // local results — see filter.geoRelType above. Distributed sources applied
  // the forwarded georel themselves. No broker-side post-filter is needed.

  // § 4.18 / § 6.18.3.2: scopeQ — applied against the entity's CURRENT
  // state (looked up from the current-state DB). Temporal entities that no
  // longer exist in current state can't satisfy a current-state filter and
  // are dropped.
  if (corNgsild.scopeExpr != NULL && db.entityRetrieve != NULL)
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
        keep = ldEntityMatchScope(scopeP, corNgsild.scopeExpr);
      }

      if (!keep)
        kjChildRemove(result, ep);

      ep = nextEp;
    }
  }

  // § 4.23: orderBy. For temporal output the sorter picks the most-recent
  // instance value per entity (see ldOrderSort.c::temporalLatestValue).
  if (corNgsild.orderByV != NULL && corNgsild.orderByCount > 0)
    ldOrderSort(result, corNgsild.orderByV, corNgsild.orderByCount, corNgsild.collation);

  // § 4.5.4 / § 4.5.5: pick/omit attribute projection (lang reduction
  // and ?format=temporalValues run in renderHook, see ldHooks.c).
  if (corNgsild.pickV != NULL || corNgsild.omitV != NULL)
  {
    for (KjNode* ep = result->value.firstChildP; ep != NULL; ep = ep->next)
      ldPickOmit(ep, corNgsild.pickV, corNgsild.omitV);
  }

  corRest.out.responseTree = result;

  // § 6.4.6 / § 6.4.7.2 general pagination — total matching ENTITIES
  // (NGSILD-Results-Count) and Link rel="next"/"prev". These coexist with
  // the temporal-interval links below (distinguished by rel, RFC 8288).
  if (corNgsild.count)
  {
    char* countStr = (char*) kaAlloc(&corRest.kalloc, 32);
    snprintf(countStr, 32, "%ld", (rangeInfo.entityCount >= 0) ? rangeInfo.entityCount : 0L);
    corRestOutHeaderAdd("NGSILD-Results-Count", countStr);
  }
  // § 7.4.2.2: no prev/next pointers for a page that is empty AND has nothing
  // more pending. When more entities remain (moreEntities) the next pointer is
  // kept even on an empty page (e.g. limit=0&count=true) so the client can
  // advance to the first data page.
  if ((result != NULL && result->value.firstChildP != NULL) || rangeInfo.moreEntities)
    ldPaginationLinkHeader(rangeInfo.moreEntities);

  // § 6.4.7.3: when any entity's instances remain beyond the returned
  // page, emit Link rel="intervalafter"/"intervalbefore" page pointers.
  ldTemporalPaginationLinkHeader(rangeInfo.hasMore, rangeInfo.size);
  corRest.out.httpStatusCode = 200;

  return true;
}

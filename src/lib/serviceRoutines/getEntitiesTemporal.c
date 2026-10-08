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
#include <pthread.h>                                 // pthread_rwlock_rdlock

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/corRestOutHeader.h"                  // corRestOutHeaderAdd
#include "corRest/corRestUrlValueEncode.h"             // corRestUrlValueEncode
#include "corNgsild/ldPagination.h"                   // ldTemporalPaginationLinkHeader
#include "corNgsild/ldToAggregatedValues.h"           // ldAggrMethodValid, ldIso8601DurationParse, LdDuration
#include "corNgsild/LdRegCache.h"                     // LdRegCache
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeBuilder.h"                  // corTreeArray, corTreeChildAdd, corTreeChildRemove
#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corJson/corJsonParse.h"                    // corJsonParse
#include "corTree/corTreeClone.h"                    // corTreeClone
#include "corAlloc/corAlloc.h"                       // corAlloc
#include "corAlloc/CorAlloc.h"                         // CorAlloc

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
#include "corNgsild/ldTermId.h"                       // ldTermId, CorTerm*

#include "troe/TroeDriver.h"                         // troe, TroeQueryFilter, TroeRangeInfo
#include "troe/troeQTreeToSql.h"                     // troeQTreeToSql, troeQTreeToRowSql
#include "troe/troeNotAvailable.h"                   // troeNotAvailable

#include "db/DbDriver.h"                             // db
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/ldSnapshotRead.h"          // ldSnapshotItemFromHeader
#if COR_FEATURE_RESPONSE_BUDGET
#include "serviceRoutines/responseBudget.h"          // responseBudgetBytes, responseBudgetTooMany, responseBudgetDepth, responseBudgetCut
#endif
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
// EntityTemporal (CorArray-of-instances per attr). Wildcard (no propertyNames /
// relationshipNames) strips all non-keyword children.
//
static void stripInfoAttrsFromEntity(CorNode* entityP, LdRegInfo* riP)
{
  if (entityP == NULL || entityP->type != CorObject)
    return;

  bool wildcard = (riP->attributeNamesV == NULL);

  CorNode* curP = entityP->value.head;
  while (curP != NULL)
  {
    CorNode* nextP = curP->next;

    if (curP->name != NULL && curP->name[0] != '@' &&
        ldTermId(curP) != CorTermId &&
        ldTermId(curP) != CorTermType)
    {
      bool covered = wildcard;
      if (!covered && riP->attributeNamesV != NULL)
        for (int j = 0; riP->attributeNamesV[j] != NULL; j++)
          if (strcmp(curP->name, riP->attributeNamesV[j]) == 0) { covered = true; break; }

      if (covered)
        corTreeChildRemove(entityP, curP);
    }

    curP = nextP;
  }
}



// -----------------------------------------------------------------------------
//
// mergeTemporalEntity - merge upstream's per-attr instance arrays into destP.
//
// For temporal entities each non-keyword child is a CorArray of instance
// objects. "Merge" = "for each upstream attr, append its instances onto
// destP's same-named attr (creating it if absent)".
//
// keepOnlyMissing = true → auxiliary mode (§ 4.3.6.2): only copy attrs the
// dest doesn't have yet. Auxiliary registrations fill gaps; they never
// augment data already present.
//
static void mergeTemporalEntity(CorNode* destP, CorNode* upP, bool keepOnlyMissing)
{
  if (destP == NULL || upP == NULL || destP->type != CorObject || upP->type != CorObject)
    return;

  CorNode* upChild = upP->value.head;
  while (upChild != NULL)
  {
    CorNode* upNext = upChild->next;

    if (upChild->name == NULL ||
        upChild->name[0] == '@' ||
        ldTermId(upChild) == CorTermId ||
        ldTermId(upChild) == CorTermType)
    {
      upChild = upNext;
      continue;
    }

    CorNode* destAttr = corTreeLookup(destP, upChild->name);

    if (destAttr == NULL)
    {
      upChild->next = NULL;
      corTreeChildAdd(destP, upChild);
    }
    else if (!keepOnlyMissing && upChild->type == CorArray && destAttr->type == CorArray)
    {
      CorNode* inst = upChild->value.head;
      while (inst != NULL)
      {
        CorNode* instNext = inst->next;
        inst->next = NULL;
        corTreeChildAdd(destAttr, inst);
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
static const char* buildTemporalQueryQs(CorAlloc* kaP)
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

  char* buf = (char*) corAlloc(kaP, len);
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
static CorNode* findEntityById(CorNode* arrayP, const char* id)
{
  if (arrayP == NULL || arrayP->type != CorArray || id == NULL)
    return NULL;

  for (CorNode* ep = arrayP->value.head; ep != NULL; ep = ep->next)
  {
    CorNode* idP = corTreeLookup(ep, "id");
    if (idP != NULL && idP->type == CorString && strcmp(idP->value.s, id) == 0)
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
static void mergeRemoteArray(CorNode* arrayP, CorNode* remoteArrayP, bool keepOnlyMissing)
{
  if (arrayP == NULL || remoteArrayP == NULL || remoteArrayP->type != CorArray)
    return;

  CorNode* remEntity = remoteArrayP->value.head;
  while (remEntity != NULL)
  {
    CorNode* nextRem = remEntity->next;

    CorNode* idP = corTreeLookup(remEntity, "id");
    if (idP == NULL || idP->type != CorString)
    {
      remEntity = nextRem;
      continue;
    }

    CorNode* localEntity = findEntityById(arrayP, idP->value.s);
    if (localEntity == NULL)
    {
      remEntity->next = NULL;
      corTreeChildAdd(arrayP, remEntity);
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
static void stripInfoAttrsFromArray(CorNode* arrayP, LdRegInfo* riP)
{
  if (arrayP == NULL || arrayP->type != CorArray)
    return;

  for (CorNode* ep = arrayP->value.head; ep != NULL; ep = ep->next)
  {
    CorNode* idP = corTreeLookup(ep, "id");
    if ((idP != NULL) && (idP->type == CorString) && !entityInfoCoversId(riP, idP->value.s))
      continue;

    stripInfoAttrsFromEntity(ep, riP);
  }
}



// -----------------------------------------------------------------------------
//
// distOpsPossible - can a registration contribute to this query's answer?
//
// The same gate as the distop dispatch below, plus: an empty registration cache
// cannot contribute either.
//
static bool distOpsPossible(void* snapItem, Tenant* tenantP)
{
  if ((snapItem != NULL) || corNgsild.local || (tenantP == NULL) || (tenantP->regCacheP == NULL))
    return false;

  LdRegCache* cacheP = (LdRegCache*) tenantP->regCacheP;

  pthread_rwlock_rdlock(&cacheP->lock);
  bool registrations = (cacheP->itemList != NULL);
  pthread_rwlock_unlock(&cacheP->lock);

  return registrations;
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

  // § 11.3.3.4: at least one of the clause's CLOSED list must be present -
  // type / attrs / q / GeoQuery / local scope. Identical to § 10.4.3.4 for
  // /entities, and identically NOT satisfied by a list of entity ids:
  // "it is not possible to retrieve a set of entities by only specifying
  // desired Entity identifiers". ETSI 021_24 asserts the 400.
  if (corNgsild.typeV == NULL
      && corNgsild.attrsV == NULL && corNgsild.qExpr == NULL && corNgsild.georel == NULL
      && corNgsild.local == false)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Query Too Broad",
            "at least one of 'type', 'attrs', 'q', 'georel', or 'local' must be supplied - a list of entity ids is not enough");
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
    filter.qRowPredicate = troeQTreeToRowSql(corNgsild.qExpr, &corRest.kalloc);
    filter.qTree         = corNgsild.qExpr;

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

  //
  // § 4.5.20 aggregatedValues: let the store aggregate, instead of fetching
  // every raw instance for the renderHook to bucket - for a type-wide query over
  // a day of samples that is the difference between a few thousand rows and
  // millions. Only when nothing below needs the raw instances: orderBy sorts on
  // them, and a remote source's answer is merged instance by instance. The
  // plugin can still decline (and does, for anything it cannot reproduce
  // exactly); the renderHook then aggregates as always.
  //
  if ((corNgsild.format == LdFormatAggregatedValues) && (corNgsild.orderByV == NULL) && !distOpsPossible(snapItem, tenantP))
  {
    LdDuration period = { 0, 0 };

    if ((corNgsild.aggrPeriodDuration == NULL) || ldIso8601DurationParse(corNgsild.aggrPeriodDuration, &period))
    {
      filter.aggrPushdown     = true;
      filter.aggrMethodsV     = corNgsild.aggrMethodsV;
      filter.aggrPeriodMonths = period.months;
      filter.aggrPeriodNs     = period.ns;
      filter.timeAtNs         = corNgsild.timeAtNs;
      filter.endTimeAtNs      = corNgsild.endTimeAtNs;
    }
  }

#if COR_FEATURE_RESPONSE_BUDGET
  //
  // The byte budget (--maxResponseSize, TroeDriver.h): the store's page ends before the entity that
  // does not fit, and a first entity that does not fit whole comes with fewer instances per attribute
  //
  filter.maxBytes = responseBudgetBytes;
#endif

  TroeRangeInfo rangeInfo;
  memset(&rangeInfo, 0, sizeof(rangeInfo));

  CorNode* result = NULL;
  int     r      = troe.entityTemporalQuery(tenantP, &filter, &result, &rangeInfo);

  if (r != TROE_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "temporal query failed");
    return true;
  }

#if COR_FEATURE_RESPONSE_BUDGET
  if (filter.budgetHit && ((result == NULL) || (result->value.head == NULL)))
  {
    responseBudgetTooMany("the next entity's temporal representation, even with one instance per attribute,");
    return true;
  }

  int nextOffset = corNgsild.offset + filter.budgetFetched;   // where the page stopped - read only when the budget ended it
#endif

  // No matches yet → start with empty array; distops may still contribute.
  if (result == NULL)
    result = corTreeArray(corRest.kallocP, NULL);

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

      LdDistOpBatchItem*   items     = (LdDistOpBatchItem*)   corAlloc(&corRest.kalloc, total * sizeof(LdDistOpBatchItem));
      memset(items, 0, total * sizeof(LdDistOpBatchItem));
      LdDistOpBatchResult* results   = (LdDistOpBatchResult*) corAlloc(&corRest.kalloc, total * sizeof(LdDistOpBatchResult));
      int*                 itemMode  = (int*)                 corAlloc(&corRest.kalloc, total * sizeof(int));
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
          char* url = (char*) corAlloc(&corRest.kalloc, baseLen + pathLen + 1 + qsLen + 1);
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
#if COR_FEATURE_RESPONSE_BUDGET
        //
        // The byte budget over the sources' answers. Each is read up to the budget and no further (a
        // body past it is dropped unread, tooLarge); then the local page and every source's page are
        // cut at the same depth - the most entities of each that fit in the budget together (and in
        // limit) - so that offset + that depth is where every one of them continues
        // (responseBudget.h). A source whose answer could not be read leaves nothing to cut: refused.
        //
        ldDistOpSendMultiMax(items, itemCount, CorVerbGet, ownAlias, results, responseBudgetBytes);

        if (responseBudgetBytes > 0)
        {
          CorNode** partV = (CorNode**) corAlloc(&corRest.kalloc, (itemCount + 1) * sizeof(CorNode*));
          int       parts = 0;

          partV[parts++] = result;

          for (int i = 0; i < itemCount; i++)
          {
            if (results[i].tooLarge)
            {
              char what[512];
              snprintf(what, sizeof(what), "the page of Context Source '%s' (lower 'limit')",
                       (items[i].csr->regId != NULL) ? items[i].csr->regId : "?");
              responseBudgetTooMany(what);
              for (int m = 0; m < 4; m++)
                ldRegCacheMatchRelease(matchV[m], matchN[m]);
              return true;
            }

            if ((results[i].statusCode >= 200) && (results[i].statusCode < 300) &&
                (results[i].responseTree != NULL) && (results[i].responseTree->type == CorArray))
              partV[parts++] = results[i].responseTree;
          }

          int localN = 0;
          for (CorNode* eP = result->value.head; eP != NULL; eP = eP->next)
            ++localN;

          int pageLimit = limitGiven ? corNgsild.limit : 1000;   // as the store's page (no limit: 1000)
          int depth     = responseBudgetDepth(partV, parts, responseBudgetBytes, pageLimit, filter.budgetHit ? localN : -1);

          if (depth == 0)
          {
            responseBudgetTooMany("the next entity of this broker together with the next one of each Context Source");
            for (int m = 0; m < 4; m++)
              ldRegCacheMatchRelease(matchV[m], matchN[m]);
            return true;
          }

          if (depth > 0)
          {
            responseBudgetCut(partV, parts, depth);
            filter.budgetHit       = true;
            rangeInfo.moreEntities = true;
            nextOffset             = corNgsild.offset + depth;
          }
        }
#else
        ldDistOpSendMulti(items, itemCount, CorVerbGet, ownAlias, results);
#endif

        for (int i = 0; i < itemCount; i++)
        {
          int upCode = results[i].statusCode;
          if (upCode < 200 || upCode >= 300) continue;
          if (results[i].responseBody == NULL || results[i].responseBodyLen == 0) continue;

          CorNode* remoteArray = results[i].responseTree;
          if (remoteArray == NULL || remoteArray->type != CorArray) continue;

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
    CorNode* ep = result->value.head;
    while (ep != NULL)
    {
      CorNode* nextEp = ep->next;
      CorNode* idP   = corTreeLookup(ep, "id");
      bool    keep   = false;

      CorNode* curEntity = NULL;
      if (idP != NULL && idP->type == CorString)
        db.entityRetrieve(tenantP, idP->value.s, &curEntity);

      if (curEntity != NULL)
      {
        CorNode* scopeP = corTreeLookup(curEntity, "scope");
        keep = ldEntityMatchScope(scopeP, corNgsild.scopeExpr);
      }

      if (!keep)
        corTreeChildRemove(result, ep);

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
    for (CorNode* ep = result->value.head; ep != NULL; ep = ep->next)
      ldPickOmit(ep, corNgsild.pickV, corNgsild.omitV);
  }

  corRest.out.responseTree = result;

  // § 6.4.6 / § 6.4.7.2 general pagination — total matching ENTITIES
  // (NGSILD-Results-Count) and Link rel="next"/"prev". These coexist with
  // the temporal-interval links below (distinguished by rel, RFC 8288).
  if (corNgsild.count)
  {
    char* countStr = (char*) corAlloc(&corRest.kalloc, 32);
    snprintf(countStr, 32, "%ld", (rangeInfo.entityCount >= 0) ? rangeInfo.entityCount : 0L);
    corRestOutHeaderAdd("NGSILD-Results-Count", countStr);
  }
  // § 7.4.2.2: no prev/next pointers for a page that is empty AND has nothing
  // more pending. When more entities remain (moreEntities) the next pointer is
  // kept even on an empty page (e.g. limit=0&count=true) so the client can
  // advance to the first data page.
#if COR_FEATURE_RESPONSE_BUDGET
  //
  // A page the budget ended: the next one starts where it stopped, not `limit` further on. An entity
  // cut to fewer instances ends its page too (the store took one position), and its instances go on
  // in the temporal pagination Link below.
  //
  if (filter.budgetHit)
    ldPaginationLinkHeaderAt(rangeInfo.moreEntities, nextOffset);
  else if ((result != NULL && result->value.head != NULL) || rangeInfo.moreEntities)
    ldPaginationLinkHeader(rangeInfo.moreEntities);
#else
  if ((result != NULL && result->value.head != NULL) || rangeInfo.moreEntities)
    ldPaginationLinkHeader(rangeInfo.moreEntities);
#endif

  // § 6.4.7.3: when any entity's instances remain beyond the returned
  // page, emit Link rel="intervalafter"/"intervalbefore" page pointers.
  ldTemporalPaginationLinkHeader(rangeInfo.hasMore, rangeInfo.size);
  corRest.out.httpStatusCode = 200;

  return true;
}

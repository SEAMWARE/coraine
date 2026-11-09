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
#include "corNgsild/ldQRender.h"                      // ldQRender, ldCompactOrEncode

#include "troe/TroeDriver.h"                         // troe, TroeQueryFilter, TroeRangeInfo
#include "troe/troeQTreeToSql.h"                     // troeQTreeToSql, troeQTreeToRowSql
#include "troe/troeNotAvailable.h"                   // troeNotAvailable

#include "db/DbDriver.h"                             // db
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/ldSnapshotRead.h"          // ldSnapshotItemFromHeader
#if COR_FEATURE_RESPONSE_BUDGET
#include "serviceRoutines/responseBudget.h"          // responseBudgetBytes, responseBudgetTooMany, responseBudgetDepth, responseBudgetCut
#endif
#if COR_FEATURE_AUTH
#include "auth/auth.h"                               // authEntityArrayFilter
#endif
#if COR_FEATURE_TEMPORAL_ENTITY_MAP
#include "corHash/corHash.h"                         // CorHashTable, corHashTableCreate, corHashItemAdd, corHashItemLookup
#include "corNgsild/LdEntityMap.h"                    // LdEntityMap, LdEntityMapStore
#include "corNgsild/ldEntityMap.h"                    // ldEntityMapCreate, ldEntityMapAdmit, ldEntityMapToTree, ldEntityMapQueryParam[Add], ldEntityMapLookupPinned
#include "corNgsild/ldCheckDateTime.h"                // ldIsoToNanoseconds
#include "corJson/corJsonRenderSize.h"               // corJsonFastRenderSize
#include "serviceRoutines/getEntities.h"             // bindEntityMapFilters
#include "serviceRoutines/entityMapBuild.h"          // entityMapFill, entityMapHeader
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
// qsAdd - key=value onto a query string (the value URL-encoded); a NULL value adds nothing
//
static char* qsAdd(char* qs, const char* key, const char* value)
{
  if ((value == NULL) || (value[0] == 0))
    return qs;

  const char* enc  = corRestUrlValueEncode(value, &corRest.kalloc);
  int         size = strlen(qs) + strlen(key) + strlen(enc) + 3;
  char*       out  = (char*) corAlloc(&corRest.kalloc, size);

  snprintf(out, size, "%s%s%s=%s", qs, (qs[0] != 0) ? "&" : "", key, enc);
  return out;
}



// -----------------------------------------------------------------------------
//
// nameList - a list of IRIs as the comma separated list of a URL parameter, each compacted with the
// forward @context (one it does not compact stays the IRI)
//
static char* nameList(char** v, CorLdContext* ctxP)
{
  if ((v == NULL) || (v[0] == NULL))
    return NULL;

  int n = 1;
  for (int ix = 0; v[ix] != NULL; ix++)
    n += strlen(ldCompactOrEncode(v[ix], ctxP, &corRest.kalloc, false)) + 1;

  char* out = (char*) corAlloc(&corRest.kalloc, n);
  int   pos = 0;

  for (int ix = 0; v[ix] != NULL; ix++)
  {
    const char* s = ldCompactOrEncode(v[ix], ctxP, &corRest.kalloc, false);

    if (ix > 0)
      out[pos++] = ',';

    strcpy(&out[pos], s);
    pos += strlen(s);
  }

  out[pos] = 0;
  return out;
}



// -----------------------------------------------------------------------------
//
// parsedQueryQs - the query string a Context Source is asked with, built from the query AS PARSED, not from
// the URL: a POST's Query is in the body (ldQueryBodyCheck turned it into the parsed fields). The names are
// compacted with the @context the forward carries (its Link header). ids: the entities asked for (a comma
// separated list) instead of the request's id / idPattern.
//
// filters false: the entity selector and the temporal window only - q, the geo-query, scopeQ and attrs left
// out (split entities, TS 104-175 § 14.6.4: the source answers the candidates). scopeQ false: not sent even
// with the filters (Query Temporal applies it on the merged result).
//
static const char* parsedQueryQs(LdRegCacheItem* csr, const char* ids, bool filters, bool scopeQ)
{
  CorLdContext* ctxP = ldDistOpForwardContext(csr);
  char*         qs   = (char*) "";

  if (ids != NULL)                    // a page of an EntityMap: its entities, not the request's selector
    qs = qsAdd(qs, "id", ids);
  else
  {
    qs = qsAdd(qs, "id",        (corNgsild.idV != NULL) ? corNgsild.id : NULL);
    qs = qsAdd(qs, "idPattern", corNgsild.idPattern);
  }

  qs = qsAdd(qs, "type",         nameList(corNgsild.typeV, ctxP));
  qs = qsAdd(qs, "timerel",      corNgsild.timerel);
  qs = qsAdd(qs, "timeAt",       corNgsild.timeAt);
  qs = qsAdd(qs, "endTimeAt",    corNgsild.endTimeAt);
  qs = qsAdd(qs, "timeproperty", corNgsild.timeproperty);
  qs = qsAdd(qs, "datasetId",    corNgsild.datasetId);

  if (filters == true)
  {
    qs = qsAdd(qs, "attrs",  nameList(corNgsild.attrsV, ctxP));
    qs = qsAdd(qs, "q",      (corNgsild.qExpr != NULL) ? ldQRender(corNgsild.qExpr, ctxP, &corRest.kalloc, false) : NULL);

    if (scopeQ == true)
      qs = qsAdd(qs, "scopeQ", corNgsild.scopeQ);

    if (corNgsild.georel != NULL)
    {
      qs = qsAdd(qs, "georel",      corNgsild.georel);
      qs = qsAdd(qs, "geometry",    corNgsild.geometry);
      qs = qsAdd(qs, "coordinates", corNgsild.coordinates);

      if (corNgsild.geoproperty != NULL)
        qs = qsAdd(qs, "geoproperty", ldCompactOrEncode(corNgsild.geoproperty, ctxP, &corRest.kalloc, false));
    }
  }

  return qs;
}



// -----------------------------------------------------------------------------
//
// buildTemporalQueryQs - rebuild the forwarded query string.
//
// Forwards every URL param except output-shaping ones (pick/omit/lang/format/
// orderBy/options/local/entityMap). Filters that constrain the candidate set
// (id/idPattern/type/q/timerel/timeAt/.../georel/...) all forward verbatim.
//
// queryInBody: a POST (/temporal/entityOperations/query) - the query is in the body, not the URL: the
// keys parsedQueryQs builds from the parsed query are skipped here, the caller adds that part.
//
// mapPage: the request to a source for its entities of a page of an EntityMap - its parsed part
// (parsedQueryQs, with the page's ids) the caller's, and no paging: not limit, offset, count, the map's
// own parameters (splitEntities, entityMapLifetime).
//
static bool parsedKey(const char* k)
{
  static const char* keys[] = { "id", "idPattern", "type", "attrs", "q", "timerel", "timeAt", "endTimeAt", "timeproperty",
                                "datasetId", "georel", "geometry", "coordinates", "geoproperty", NULL };

  for (int ix = 0; keys[ix] != NULL; ix++)
  {
    if (strcmp(k, keys[ix]) == 0)
      return true;
  }

  return false;
}

static bool pagingKey(const char* k)
{
  return (strcmp(k, "limit") == 0) || (strcmp(k, "offset") == 0) || (strcmp(k, "count") == 0) ||
         (strcmp(k, "splitEntities") == 0) || (strcmp(k, "entityMapLifetime") == 0);
}

static const char* buildTemporalQueryQs(CorAlloc* kaP, bool queryInBody, bool mapPage)
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
    if ((queryInBody || mapPage) && parsedKey(k)) continue;  // from the parsed query - parsedQueryQs
    if (mapPage && pagingKey(k)) continue;

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
    if ((queryInBody || mapPage) && parsedKey(k)) continue;  // from the parsed query - parsedQueryQs
    if (mapPage && pagingKey(k)) continue;

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
    if ((idP != NULL) && (idP->type == CorString) && !ldRegInfoCoversEntity(riP, idP->value.s, NULL))
      continue;

    stripInfoAttrsFromEntity(ep, riP);
  }
}



// -----------------------------------------------------------------------------
//
// scopeFilter - the entities of arrayP whose Scope (their current one, from the store) does not match
// scopeQ are taken out
//
static void scopeFilter(CorNode* arrayP, Tenant* tenantP)
{
  if ((corNgsild.scopeExpr == NULL) || (db.entityRetrieve == NULL))
    return;

  CorNode* ep = arrayP->value.head;
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
      corTreeChildRemove(arrayP, ep);

    ep = nextEp;
  }
}



#if COR_FEATURE_TEMPORAL_ENTITY_MAP
// -----------------------------------------------------------------------------
//
// idHash / srcCompare - a hash of srcMap's arrays, by entity id (the array's name)
//
static unsigned int idHash(const char* name)
{
  unsigned int h = 5381;

  while (*name != 0)
    h = (h * 33) ^ (unsigned char) *name++;

  return h;
}

static int srcCompare(const char* name, void* itemP)
{
  return strcmp(name, ((CorNode*) itemP)->name);
}



// -----------------------------------------------------------------------------
//
// temporalWindowRecord - the temporal window of the query into the map, with its other parameters (the URL's:
// ldEntityMapSetQueryParams): a body's temporalQ is in no URL
//
static const char* windowKeyV[] = { "timerel", "timeAt", "endTimeAt", "timeproperty", NULL };

static const char** windowValue(int ix)
{
  switch (ix)
  {
  case 0:  return (const char**) &corNgsild.timerel;
  case 1:  return (const char**) &corNgsild.timeAt;
  case 2:  return (const char**) &corNgsild.endTimeAt;
  default: return (const char**) &corNgsild.timeproperty;
  }
}

static void temporalWindowRecord(LdEntityMap* mapP)
{
  for (int ix = 0; windowKeyV[ix] != NULL; ix++)
    ldEntityMapQueryParamAdd(mapP, windowKeyV[ix], *windowValue(ix));
}



// -----------------------------------------------------------------------------
//
// temporalWindowBind - a page of a map: its temporal window the map's
//
// A parameter of the window that differs from the map's is 400 (§ 9.6, "the same parameters"); one left out
// is the map's. A map made by a query that was not temporal (/entityMaps) has none: the page's own.
// Returns true when answered (the 400).
//
static bool temporalWindowBind(LdEntityMap* mapP)
{
  for (int ix = 0; windowKeyV[ix] != NULL; ix++)
  {
    const char*  bound  = ldEntityMapQueryParam(mapP, windowKeyV[ix]);
    const char** valueP = windowValue(ix);

    if (bound == NULL)
      continue;

    if (*valueP == NULL)
      *valueP = bound;
    else if (strcmp(*valueP, bound) != 0)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "URL parameter '%s' differs from the query that created entity map '%s'", windowKeyV[ix], corNgsild.entityMapId);
      return true;
    }
  }

  if (corNgsild.timeAt != NULL)
    corNgsild.timeAtNs = ldIsoToNanoseconds(corNgsild.timeAt);
  if (corNgsild.endTimeAt != NULL)
    corNgsild.endTimeAtNs = ldIsoToNanoseconds(corNgsild.endTimeAt);

  return false;
}



// -----------------------------------------------------------------------------
//
// temporalEntityMap - Create EntityMap for Query Temporal Evolution of Entities (TS 104-175 § 14.6)
//
// The query has been validated as a temporal query is; fP is its store filter. The map is the ids of
// the matching entities - the local ones from the store (its entity selector, read page by page, no
// history), then those of every Context Source whose registration matches and supports
// createEntityMapQueryTemporal: each is asked for an EntityMap of its own (GET /temporal/entityMaps),
// whose id goes into linkedMaps and whose entities into entityMap, with the source's registration.
//
// Split entities (the deployment's default, or splitEntities=true), with a source to ask: the filters
// are left out, locally as well as at the sources (§ 14.6.4) - the map holds the candidates.
//
// Answered as GET and POST /entityMaps answer: 201, the map, and NGSILD-EntityMap. limit and offset
// are ignored (§ 14.6.3): a map is never a page.
//
// servePP: ?entityMap=true on Query Temporal - the map made the same way, not answered: *servePP, for the
// caller to serve its first page from (false: go on; true: answered, an error).
//
// The temporal window goes into the map with the rest of the query (temporalWindowRecord): a page of it
// names the map only, and is served in the window the map was made in.
//
static bool temporalEntityMap(Tenant* tenantP, TroeQueryFilter* fP, LdEntityMap** servePP)
{
  Tenant* mapTenantP = (Tenant*) corNgsild.tenantP;

  if ((mapTenantP == NULL) || (mapTenantP->entityMapStoreP == NULL))
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "the tenant has no EntityMap store");
    return true;
  }

#if COR_FEATURE_AUTH
  //
  // The map is the ids of every matching entity - not decided entity by entity: refused
  //
  if (authEntityWanted() == true)
  {
    authEntityDeny("an EntityMap names entities without asking about each - query the entities instead");
    return true;
  }
#endif

  //
  // The Context Sources to ask - before the local query: whether one matches decides split
  //
  const char*      ownAlias = NULL;
  LdRegCacheItem** matchV[4] = { NULL, NULL, NULL, NULL };
  int              matchN[4] = { 0, 0, 0, 0 };
  int              total     = 0;

  if (!corNgsild.local && (tenantP != NULL) && (tenantP->regCacheP != NULL) && (tenantP == mapTenantP))
  {
    ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);

    if (!ldDistOpLoopDetected(ownAlias))
    {
      LdRegMode modes[] = { LdRegModeExclusive, LdRegModeRedirect, LdRegModeInclusive, LdRegModeAuxiliary };

      for (int m = 0; m < 4; m++)
      {
        const char** typeFilterV = (modes[m] == LdRegModeAuxiliary) ? NULL : (const char**) corNgsild.typeV;

        matchN[m] = ldRegCacheMatchForQuery((LdRegCache*) tenantP->regCacheP, corNgsild.idV, corNgsild.idPattern,
                                            (char**) typeFilterV, modes[m], &matchV[m]);
        total += matchN[m];
      }
    }
  }

  LdDistOpBatchItem*   items     = (total > 0) ? (LdDistOpBatchItem*)   corAlloc(&corRest.kalloc, total * sizeof(LdDistOpBatchItem))   : NULL;
  LdDistOpBatchResult* results   = (total > 0) ? (LdDistOpBatchResult*) corAlloc(&corRest.kalloc, total * sizeof(LdDistOpBatchResult)) : NULL;
  int                  itemCount = 0;

  for (int m = 0; m < 4; m++)
  {
    for (int i = 0; i < matchN[m]; i++)
    {
      LdRegCacheItem* csr = matchV[m][i];

      if (csr->endpoint == NULL)                                   continue;
      if (!ldRegOpSupported(csr, LdOpCreateEntityMapQueryTemporal)) continue;
      if (ldDistOpCsrWouldLoop(csr, ownAlias))                     continue;
      if (!csfCsrMatch(csr))                                       continue;

      memset(&items[itemCount],   0, sizeof(LdDistOpBatchItem));
      memset(&results[itemCount], 0, sizeof(LdDistOpBatchResult));
      items[itemCount].csr = csr;   // the URL below, once split is known
      ++itemCount;
    }
  }

  bool split = (itemCount > 0) && (corNgsild.splitEntitiesSet ? corNgsild.splitEntitiesVal : ldSplitEntities);

  //
  // The local entities: the store's selector, page by page, ids only
  //
  if (split)
  {
    fP->qSqlPredicate  = NULL;
    fP->qRowPredicate  = NULL;
    fP->qTree          = NULL;
    fP->geoRelType     = 0;
    fP->geoGeometry    = NULL;
    fP->geoCoordinates = NULL;
    fP->geoProperty    = NULL;
  }

  fP->idsOnly      = true;
  fP->limit        = 10000;
  fP->limitGiven   = true;
  fP->offset       = 0;
  fP->count        = false;
  fP->lastN        = 1;      // a store that answers the history anyway: as little of it as possible
  fP->firstN       = 0;
  fP->offsetN      = 0;
  fP->aggrPushdown = false;
#if COR_FEATURE_RESPONSE_BUDGET
  fP->maxBytes     = 0;      // what bounds a map is the maps' memory (ldEntityMapAdmit)
#endif

  CorNode*      idArrayP = corTreeArray(corRest.kallocP, NULL);
  CorNode*      srcMap   = corTreeObject(corRest.kallocP, NULL);
  CorNode*      linkedP  = corTreeObject(corRest.kallocP, NULL);
  CorHashTable* srcHashP = corHashTableCreate(&corRest.kalloc, idHash, srcCompare, 1024);

  while (1)
  {
    TroeRangeInfo rangeInfo;
    CorNode*      pageP = NULL;

    memset(&rangeInfo, 0, sizeof(rangeInfo));

    if (troe.entityTemporalQuery(tenantP, fP, &pageP, &rangeInfo) != TROE_OK)
    {
      ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "temporal query failed");
      for (int m = 0; m < 4; m++)
        ldRegCacheMatchRelease(matchV[m], matchN[m]);
      return true;
    }

    CorNode* eP = (pageP != NULL) ? pageP->value.head : NULL;
    while (eP != NULL)
    {
      CorNode* nextP = eP->next;
      CorNode* idP   = corTreeLookup(eP, "id");

      if ((idP != NULL) && (idP->type == CorString))
      {
        CorNode* entryP = corTreeObject(corRest.kallocP, NULL);
        corTreeChildAdd(entryP, corTreeString(corRest.kallocP, "id", idP->value.s));
        corTreeChildAdd(idArrayP, entryP);
      }

      eP = nextP;
    }

    if (rangeInfo.moreEntities == false)
      break;

    fP->offset += fP->limit;
  }

  if (split == false)
    scopeFilter(idArrayP, tenantP);

  for (CorNode* eP = idArrayP->value.head; eP != NULL; eP = eP->next)
  {
    const char* id   = corTreeLookup(eP, "id")->value.s;
    CorNode*    srcP = corTreeArray(corRest.kallocP, id);

    corTreeChildAdd(srcP, corTreeString(corRest.kallocP, NULL, "@none"));
    corTreeChildAdd(srcMap, srcP);
    corHashItemAdd(srcHashP, srcP->name, srcP);
  }

  //
  // The Context Sources' maps
  //
  if (itemCount > 0)
  {
    for (int i = 0; i < itemCount; i++)
    {
      const char* path    = "/ngsi-ld/v1/temporal/entityMaps?";
      const char* qs      = parsedQueryQs(items[i].csr, NULL, split == false, true);
      int         size    = strlen(items[i].csr->endpoint) + strlen(path) + strlen(qs) + 1;
      char*       url     = (char*) corAlloc(&corRest.kalloc, size);

      snprintf(url, size, "%s%s%s", items[i].csr->endpoint, path, qs);
      items[i].url = url;
    }

    ldDistOpSendMultiMax(items, itemCount, CorVerbGet, ownAlias, results, ldEntityMapMaxBytes);

    for (int i = 0; i < itemCount; i++)
    {
      CorNode* treeP = results[i].responseTree;

      if ((results[i].statusCode < 200) || (results[i].statusCode >= 300) || (treeP == NULL) || (treeP->type != CorObject))
        continue;   // a source without the map: not in it (§ 9.6, a source declines by not returning one)

      CorNode*    mapIdP = corTreeLookup(treeP, "id");
      CorNode*    emP    = corTreeLookup(treeP, "entityMap");
      const char* regId  = items[i].csr->regId;

      if ((emP == NULL) || (emP->type != CorObject) || (regId == NULL))
        continue;

      if ((mapIdP != NULL) && (mapIdP->type == CorString))
        corTreeChildAdd(linkedP, corTreeString(corRest.kallocP, regId, mapIdP->value.s));

      for (CorNode* entryP = emP->value.head; entryP != NULL; entryP = entryP->next)
      {
        if (entryP->name == NULL)
          continue;

        CorNode* srcP = (CorNode*) corHashItemLookup(srcHashP, entryP->name);

        if (srcP == NULL)
        {
          CorNode* idEntryP = corTreeObject(corRest.kallocP, NULL);
          corTreeChildAdd(idEntryP, corTreeString(corRest.kallocP, "id", entryP->name));
          corTreeChildAdd(idArrayP, idEntryP);

          srcP = corTreeArray(corRest.kallocP, entryP->name);
          corTreeChildAdd(srcMap, srcP);
          corHashItemAdd(srcHashP, srcP->name, srcP);
        }

        corTreeChildAdd(srcP, corTreeString(corRest.kallocP, NULL, regId));
      }
    }
  }

  for (int m = 0; m < 4; m++)
    ldRegCacheMatchRelease(matchV[m], matchN[m]);

  //
  // The map - as GET / POST /entityMaps make it (getEntities.c)
  //
  LdEntityMapStore* storeP = (LdEntityMapStore*) mapTenantP->entityMapStoreP;

  ldEntityMapPurgeExpired(storeP);

  LdEntityMap* mapP = ldEntityMapCreate(storeP, 5ULL * 60 * 1000000000ULL, mapTenantP);   // the default lifetime: 5 minutes

  if (mapP == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "out of memory creating an EntityMap");
    return true;
  }

  ldEntityMapRequestPin(mapP);   // created pinned; held (and filled, rendered) till the request ends
  entityMapFill(mapP, idArrayP, srcMap, linkedP);
  temporalWindowRecord(mapP);
  mapP->automatic = corNgsild.entityMapAuto;

  if (ldEntityMapAdmit(storeP, mapP) == false)
  {
    ldError(403, LD_ERROR_TOO_MANY_RESULTS, "Too Many Results",
            "the %d matching entities do not fit in the memory of the EntityMaps (--entityMapMemory %lld MiB) - narrow the query",
            mapP->entryCount, (long long) (ldEntityMapMaxBytes >> 20));
    return true;
  }

  if (servePP != NULL)
  {
    *servePP = mapP;
    return false;
  }

  entityMapHeader(mapP);
  corRest.out.responseTree   = ldEntityMapToTree(mapP);
  corRest.out.httpStatusCode = 201;
  corNgsild.rawResponse      = true;

  return true;
}



// -----------------------------------------------------------------------------
//
// temporalMapOpen - ?entityMap=<id> (or the NGSILD-EntityMap request header) on Query Temporal: the map,
// pinned for the request, the page's query made the map's (§ 9.6: "the same parameters")
//
// The map's filters (bindEntityMapFilters) and its temporal window (temporalWindowBind): a parameter that
// differs from the map's is 400, one left out is the map's - a link of a page names the map only. A map
// that is gone (expired, deleted) and a request that is a complete query: a new map, made from the request
// (§ 9.6: "a new one shall be created"), *mapPP NULL and corNgsild.entityMapCreate set; a request that is
// not one: 404. Returns true when answered (an error).
//
static bool temporalMapOpen(LdEntityMap** mapPP)
{
  Tenant* tP = (Tenant*) corNgsild.tenantP;

  *mapPP = NULL;

  LdEntityMap* mapP = ((tP != NULL) && (tP->entityMapStoreP != NULL))
                        ? ldEntityMapLookupPinned((LdEntityMapStore*) tP->entityMapStoreP, corNgsild.entityMapId)
                        : NULL;

  if (mapP == NULL)
  {
    bool complete = (corNgsild.timerel != NULL) &&
                    ((corNgsild.typeV != NULL) || (corNgsild.attrsV != NULL) || (corNgsild.qExpr != NULL) ||
                     (corNgsild.georel != NULL) || (corNgsild.local == true));

    if (complete == false)
    {
      ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity map '%s' not found or expired", corNgsild.entityMapId);
      return true;
    }

    corNgsild.entityMapId     = NULL;
    corNgsild.entityMapCreate = true;
    corNgsild.entityMapAuto   = true;
    return false;
  }

  ldEntityMapRequestPin(mapP);     // unpinned when the response is out

  if (bindEntityMapFilters(mapP) == true)
    return true;

  if (temporalWindowBind(mapP) == true)
    return true;

  //
  // The map's type, put back by bindEntityMapFilters as an expression: the temporal query reads the list
  //
  if ((corNgsild.typeV == NULL) && (corNgsild.typeExpr != NULL) && corNgsild.typeExpr->isSimple)
  {
    int n = corNgsild.typeExpr->groupCount;

    corNgsild.typeV = (char**) corAlloc(&corRest.kalloc, (n + 1) * sizeof(char*));
    for (int ix = 0; ix < n; ix++)
      corNgsild.typeV[ix] = corNgsild.typeExpr->groupV[ix].typeV[0];
    corNgsild.typeV[n] = NULL;
  }

  ldEntityMapTouch(mapP);
  *mapPP = mapP;

  return false;
}



// -----------------------------------------------------------------------------
//
// temporalMapPage - a page of an EntityMap, Query Temporal Evolution of Entities: the entries
// [offset, offset + limit) of the map, each entity's history fetched from where the map says it is
//
// The local ones from the store (their ids, the query's filters and window), those of each Context Source
// the map recorded in one request per source - GET /temporal/entities?id=<its entities of the page>&...
// (100 ids a request), the registration looked up by the id the map holds, not matched again: the map is a
// frozen set. The parts of an entity merged (an entity split across sources), the page in the map's order;
// an entity with no history in the window is not on it. 200, the map in NGSILD-EntityMap, its links name
// the map.
//
static bool temporalMapPage(LdEntityMap* mapP, Tenant* tenantP, TroeQueryFilter* fP)
{
  bool limitGiven = (corRest.in.uriParamMask & LD_PARAM_LIMIT) != 0;
  int  limit      = limitGiven ? corNgsild.limit : ((corNgsild.limit > 0) ? corNgsild.limit : 20);
  int  offset     = corNgsild.offset;

  entityMapHeader(mapP);

  //
  // The slice
  //
  LdEntityMapEntry* entryP = mapP->head;

  for (int ix = 0; (entryP != NULL) && (ix < offset); ix++)
    entryP = entryP->next;

  LdEntityMapEntry** sliceV = (LdEntityMapEntry**) corAlloc(&corRest.kalloc, sizeof(LdEntityMapEntry*) * (limit + 1));
  int                sliceN = 0;

  for (; (entryP != NULL) && (sliceN < limit); entryP = entryP->next)
    sliceV[sliceN++] = entryP;

  //
  // The local entities: the store, asked for these ids
  //
  CorNode* result = NULL;
  char**   localV = (char**) corAlloc(&corRest.kalloc, sizeof(char*) * (sliceN + 1));
  int      localN = 0;

  for (int i = 0; i < sliceN; i++)
  {
    for (int s = 0; s < sliceV[i]->sourceCount; s++)
    {
      if (strcmp(sliceV[i]->sourceIdV[s], "@none") == 0)
      {
        localV[localN++] = sliceV[i]->entityId;
        break;
      }
    }
  }
  localV[localN] = NULL;

  if (localN > 0)
  {
    TroeQueryFilter local = *fP;
    TroeRangeInfo   range;

    memset(&range, 0, sizeof(range));
    local.idV          = localV;
    local.idPattern    = NULL;
    local.limit        = localN;
    local.limitGiven   = true;
    local.offset       = 0;
    local.count        = false;
    local.aggrPushdown = false;    // a source's part is merged instance by instance
    local.maxBytes     = 0;        // the budget is the page's, below

    if (troe.entityTemporalQuery(tenantP, &local, &result, &range) != TROE_OK)
    {
      ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "temporal query failed");
      return true;
    }
  }

  if (result == NULL)
    result = corTreeArray(corRest.kallocP, NULL);

  //
  // The remote ones: per registration the map recorded, its entities of the page
  //
  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);
  LdRegCache* rcP      = (LdRegCache*) tenantP->regCacheP;
  int         maxReqs  = 0;

  for (int i = 0; i < sliceN; i++)
    maxReqs += sliceV[i]->sourceCount;

  const char**       regV   = (const char**) corAlloc(&corRest.kalloc, sizeof(char*) * (maxReqs + 1));
  LdRegCacheItem**   csrV   = (LdRegCacheItem**) corAlloc(&corRest.kalloc, sizeof(LdRegCacheItem*) * (maxReqs + 1));
  int                regN   = 0;

  for (int i = 0; i < sliceN; i++)
  {
    for (int s = 0; s < sliceV[i]->sourceCount; s++)
    {
      const char* regId = sliceV[i]->sourceIdV[s];
      int         ix    = 0;

      if (strcmp(regId, "@none") == 0)
        continue;

      while ((ix < regN) && (strcmp(regV[ix], regId) != 0))
        ix++;

      if (ix == regN)
        regV[regN++] = regId;
    }
  }

  if ((regN > 0) && (rcP != NULL))
  {
    ldRegCacheRdLock(rcP);
    for (int ix = 0; ix < regN; ix++)
    {
      csrV[ix] = ldRegCacheItemLookup(rcP, regV[ix]);
      if (csrV[ix] != NULL)
        ldRegCacheItemPin(csrV[ix]);
    }
    ldRegCacheUnlock(rcP);

    const char*          fwdQs   = buildTemporalQueryQs(&corRest.kalloc, true, true);
    const char*          tpath   = "/ngsi-ld/v1/temporal/entities?";
    LdDistOpBatchItem*   items   = (LdDistOpBatchItem*)   corAlloc(&corRest.kalloc, sizeof(LdDistOpBatchItem)   * (sliceN + regN + 1));
    LdDistOpBatchResult* results = (LdDistOpBatchResult*) corAlloc(&corRest.kalloc, sizeof(LdDistOpBatchResult) * (sliceN + regN + 1));
    int                  itemN   = 0;

    memset(items,   0, sizeof(LdDistOpBatchItem)   * (sliceN + regN + 1));
    memset(results, 0, sizeof(LdDistOpBatchResult) * (sliceN + regN + 1));

    for (int ix = 0; ix < regN; ix++)
    {
      LdRegCacheItem* csr = csrV[ix];

      if ((csr == NULL) || (csr->endpoint == NULL))    // the registration is gone: its parts are not on the page
        continue;

      int i = 0;

      while (i < sliceN)
      {
        int   idsSize = 8192;
        char* ids     = (char*) corAlloc(&corRest.kalloc, idsSize);
        int   pos     = 0;
        int   n       = 0;

        ids[0] = 0;

        for (; (i < sliceN) && (n < 100); i++)
        {
          bool fromHere = false;

          for (int s = 0; s < sliceV[i]->sourceCount; s++)
            fromHere = fromHere || (strcmp(sliceV[i]->sourceIdV[s], regV[ix]) == 0);

          if (fromHere == false)
            continue;

          int len = strlen(sliceV[i]->entityId);

          if ((n > 0) && (pos + len + 2 > idsSize))
            break;                                     // the next request takes it

          pos += snprintf(ids + pos, idsSize - pos, "%s%s", (n > 0) ? "," : "", sliceV[i]->entityId);
          n++;
        }

        if (n == 0)
          continue;

        const char* parsedQs = parsedQueryQs(csr, ids, true, false);
        int         urlSize  = strlen(csr->endpoint) + strlen(tpath) + strlen(parsedQs) + strlen(fwdQs) + 2;
        char*       url      = (char*) corAlloc(&corRest.kalloc, urlSize);

        snprintf(url, urlSize, "%s%s%s%s%s", csr->endpoint, tpath, parsedQs, (fwdQs[0] != 0) ? "&" : "", fwdQs);

        items[itemN].csr = csr;
        items[itemN].url = url;
        itemN++;
      }
    }

    if (itemN > 0)
      ldDistOpSendMulti(items, itemN, CorVerbGet, ownAlias, results);

    for (int i = 0; i < itemN; i++)
    {
      CorNode* remoteArray = results[i].responseTree;

      if ((results[i].statusCode < 200) || (results[i].statusCode >= 300) || (remoteArray == NULL) || (remoteArray->type != CorArray))
        continue;                                      // a source that does not answer: its parts are not on the page

      corLdExpandTree(remoteArray, corNgsild.contextP, &corRest.kalloc);
      ldStripAtContext(remoteArray);
      mergeRemoteArray(result, remoteArray, false);
    }

    for (int ix = 0; ix < regN; ix++)
    {
      if (csrV[ix] != NULL)
        ldRegCacheItemUnpin(csrV[ix]);
    }
  }

  //
  // The page, in the map's order - and within the byte budget: it ends before the entity that would pass it
  //
  CorNode* pageP = corTreeArray(corRest.kallocP, NULL);
  int      added = 0;
#if COR_FEATURE_RESPONSE_BUDGET
  int64_t  bytes = 0;
#endif

  for (int i = 0; i < sliceN; i++)
  {
    CorNode* eP = findEntityById(result, sliceV[i]->entityId);

    if (eP != NULL)
    {
#if COR_FEATURE_RESPONSE_BUDGET
      if (responseBudgetBytes > 0)
      {
        int64_t size = corJsonFastRenderSize(eP);

        if (bytes + size > responseBudgetBytes)
        {
          if (added == 0)
          {
            responseBudgetTooMany("the next entity's temporal representation of the EntityMap");
            return true;
          }

          break;
        }

        bytes += size;
      }
#endif

      corTreeChildRemove(result, eP);
      corTreeChildAdd(pageP, eP);
    }

    added++;    // an entity of the map with no history in the window is passed, as the page's
  }

  scopeFilter(pageP, tenantP);

#if COR_FEATURE_AUTH
  if (authEntityArrayFilter(pageP) == false)       // the entities the request may not read are left out - before a projection
    return true;
#endif

  if ((corNgsild.pickV != NULL) || (corNgsild.omitV != NULL))
  {
    for (CorNode* ep = pageP->value.head; ep != NULL; ep = ep->next)
      ldPickOmit(ep, corNgsild.pickV, corNgsild.omitV);
  }

  if (corNgsild.count)
  {
    char* countStr = (char*) corAlloc(&corRest.kalloc, 32);
    snprintf(countStr, 32, "%d", mapP->entryCount);
    corRestOutHeaderAdd("NGSILD-Results-Count", countStr);
  }

  int nextOffset = offset + added;

  ldPaginationEntityMapLinkHeader(mapP, offset, limit, nextOffset, mapP->entryCount, nextOffset < mapP->entryCount);

  corRest.out.responseTree   = pageP;
  corRest.out.httpStatusCode = 200;

  return true;
}
#endif



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



// -----------------------------------------------------------------------------
//
// temporalQuery - the temporal query (mapCreate: false), or the EntityMap of one (mapCreate: true) -
// the same parameters, the same validation (TS 104-175 § 14.6.3)
//
static bool temporalQuery(bool mapCreate)
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

#if COR_FEATURE_TEMPORAL_ENTITY_MAP
  //
  // An EntityMap: ?entityMap=true makes one, ?entityMap=<id> (or NGSILD-EntityMap) pages one. Bound before
  // the checks below: a link of a page names the map, and its window and filters are the map's.
  //
  LdEntityMap* pageMapP = NULL;

  if ((mapCreate == false) && ((corNgsild.entityMapId != NULL) || corNgsild.entityMapCreate))
  {
    if (snapItem != NULL)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "an EntityMap is not used with NGSILD-Snapshot - a Snapshot is a frozen set of its own");
      return true;
    }

    if ((corNgsild.entityMapId != NULL) && (temporalMapOpen(&pageMapP) == true))
      return true;
  }
#endif

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
  if (!mapCreate && limitGiven && corNgsild.limit == 0 && corNgsild.count == false)
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

#if COR_FEATURE_TEMPORAL_ENTITY_MAP
  if (mapCreate)
    return temporalEntityMap(tenantP, &filter, NULL);

  if ((pageMapP == NULL) && corNgsild.entityMapCreate)
  {
    TroeQueryFilter mapFilter = filter;              // the map's collection rewrites its filter; the page reads this one

    if (temporalEntityMap(tenantP, &mapFilter, &pageMapP) == true)
      return true;
  }

  if (pageMapP != NULL)
    return temporalMapPage(pageMapP, tenantP, &filter);
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
      bool        queryInBody = (corRest.in.verb == CorVerbPost);
      const char* fwdQs       = buildTemporalQueryQs(&corRest.kalloc, queryInBody, false);

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

          //
          // A POST's query (the body) - per source: its names compacted with that source's forward @context
          //
          const char* qs    = fwdQs;
          int         qsN   = qsLen;

          if (queryInBody == true)
          {
            const char* bodyQs = parsedQueryQs(csr, NULL, true, false);
            int         size   = qsLen + strlen(bodyQs) + 2;
            char*       both   = (char*) corAlloc(&corRest.kalloc, size);

            snprintf(both, size, "%s%s%s", (qsLen > 0) ? fwdQs : "", ((qsLen > 0) && (bodyQs[0] != 0)) ? "&" : "", bodyQs);
            qs  = both;
            qsN = strlen(both);
          }

          int baseLen = strlen(csr->endpoint);
          char* url = (char*) corAlloc(&corRest.kalloc, baseLen + pathLen + 1 + qsN + 1);
          strcpy(url, csr->endpoint);
          strcpy(url + baseLen, tpath);
          if (qsN > 0)
          {
            url[baseLen + pathLen] = '?';
            strcpy(url + baseLen + pathLen + 1, qs);
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
  scopeFilter(result, tenantP);

  // § 4.23: orderBy. For temporal output the sorter picks the most-recent
  // instance value per entity (see ldOrderSort.c::temporalLatestValue).
  if (corNgsild.orderByV != NULL && corNgsild.orderByCount > 0)
    ldOrderSort(result, corNgsild.orderByV, corNgsild.orderByCount, corNgsild.collation);

  // § 4.5.4 / § 4.5.5: pick/omit attribute projection (lang reduction
  // and ?format=temporalValues run in renderHook, see ldHooks.c).
#if COR_FEATURE_AUTH
  if (authEntityArrayFilter(result) == false)        // the entities the request may not read are left out - before a projection
    return true;
#endif

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



// -----------------------------------------------------------------------------
//
// getEntitiesTemporal - Query Temporal Evolution of Entities (GET /temporal/entities, and the POST Query)
//
bool getEntitiesTemporal(void)
{
  return temporalQuery(false);
}



#if COR_FEATURE_TEMPORAL_ENTITY_MAP
// -----------------------------------------------------------------------------
//
// createTemporalEntityMap - GET /temporal/entityMaps: Create EntityMap for Query Temporal Evolution of
// Entities (TS 104-176 § 11.3.3.1, TS 104-175 § 14.6)
//
bool createTemporalEntityMap(void)
{
  return temporalQuery(true);
}
#endif

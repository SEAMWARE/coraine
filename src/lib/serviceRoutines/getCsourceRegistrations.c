//
// FILE            getCsourceRegistrations.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/csourceRegistrations  (NGSI-LD § 5.10.2 / § 6.8.3.2)
//
// § 5.10.2 Query Context Source Registrations — the Discovery endpoint.
//
// Matching runs against each CSR's pre-parsed `information[]` entries
// (§ 5.12). Supported filters: type, id, idPattern, and the attribute-
// name list via EITHER `pick` (current) OR `attrs` (deprecated per
// § 6.8.3.2 Table 6.8.3.2-1). Supplying both is rejected with 400.
//
// Implemented filters:
//   - type / id / idPattern        : EntityInfo matching (§ 5.12)
//   - pick (or deprecated attrs)   : at least one Property/Relationship in
//                                    RegistrationInfo matches
//   - q                            : matches CSR's own user-Properties
//                                    (organization, tier, etc. — same as csf)
//   - csf                          : alias of q above (§ 5.10.2.4)
//   - scopeQ                       : matches CSR's scope (§ 5.10.2.4)
//   - omit                         : strip listed members from the response
//   - lang                         : language reduction on response
//   - geometryProperty             : passthrough for Accept: geo+json renderer
//   - limit / offset / count       : pagination (§ 5.5.9)
//
// Deferred (501): geoQ quadruple (georel/geometry/coordinates/geoproperty)
//                 and the temporal query (timerel/timeAt/endTimeAt/
//                 timeproperty). geoQ needs GEOS-aware matching against
//                 the CSR's location/observationSpace/operationSpace and
//                 temporal needs observationInterval/managementInterval
//                 matching — both are non-trivial slices.
//
// Response: 200 OK + JSON-LD array of CSourceRegistration (§ 5.10.2.5).
//
#include <stddef.h>                                  // NULL
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // free
#include <string.h>                                  // strcmp

#include <regex.h>                                   // regcomp, regfree

#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjChildAdd, kjChildRemove
#include "kjson/kjClone.h"                           // kjClone
#include "kjson/kjLookup.h"                          // kjLookup

#include "swRest/SwRestState.h"                      // swRest
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldContextResolve
#include "swNgsild/ldCheckDateTime.h"                // ldIsoToNanoseconds
#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForDiscovery
#include "swNgsild/ldEntityMatch.h"                  // ldEntityMatchQ, ldEntityMatchScope
#include "swNgsild/ldPickOmit.h"                     // ldPickOmit
#include "swNgsild/ldLangReduce.h"                   // ldLangReduce
#include "swNgsild/ldPagination.h"                   // ldPaginationLinkHeader
#include "swNgsild/LdNormalizeInput.h"               // ldWrapAsGeoProperty
#include "swRest/swRestOutHeader.h"                  // swRestOutHeaderAdd

#include "db/DbDriver.h"                             // db, DB_OK
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getCsourceRegistrations.h" // Own interface



static bool paramSupported(const char* key)
{
  if (key == NULL)                              return true;
  if (strcmp(key, "limit")            == 0)     return true;
  if (strcmp(key, "offset")           == 0)     return true;
  if (strcmp(key, "count")            == 0)     return true;
  if (strcmp(key, "type")             == 0)     return true;
  if (strcmp(key, "id")               == 0)     return true;
  if (strcmp(key, "idPattern")        == 0)     return true;
  if (strcmp(key, "pick")             == 0)     return true;
  if (strcmp(key, "attrs")            == 0)     return true;
  if (strcmp(key, "omit")             == 0)     return true;
  if (strcmp(key, "q")                == 0)     return true;
  if (strcmp(key, "csf")              == 0)     return true;   // alias of q for CSR discovery
  if (strcmp(key, "scopeQ")           == 0)     return true;
  if (strcmp(key, "lang")             == 0)     return true;
  if (strcmp(key, "geometryProperty") == 0)     return true;   // only meaningful with Accept: geo+json
  if (strcmp(key, "georel")           == 0)     return true;   // geoQ quadruple — § 5.10.2.4
  if (strcmp(key, "geometry")         == 0)     return true;
  if (strcmp(key, "coordinates")      == 0)     return true;
  if (strcmp(key, "geoproperty")      == 0)     return true;
  if (strcmp(key, "timerel")          == 0)     return true;   // temporal query — § 5.10.2.4
  if (strcmp(key, "timeAt")           == 0)     return true;
  if (strcmp(key, "endTimeAt")        == 0)     return true;
  if (strcmp(key, "timeproperty")     == 0)     return true;
  return false;
}



// -----------------------------------------------------------------------------
//
// intervalNs - extract startAtNs / endAtNs from a CSR's TimeInterval object.
// Returns true if startAt is present (valid interval).
//
// CSRs store the interval as { startAt: <DateTime>, endAt: <DateTime>? }
// where endAt is optional ("interval still open"). Accept both KjString
// (raw ISO) and KjInt (epoch-ns) shapes.
//
static bool intervalNs(KjNode* intervalP, uint64_t* startNsP, uint64_t* endNsP)
{
  *startNsP = 0;
  *endNsP   = 0;

  if (intervalP == NULL || intervalP->type != KjObject)
    return false;

  KjNode* startP = kjLookup(intervalP, "startAt");
  if (startP == NULL)
    return false;

  if      (startP->type == KjString) *startNsP = ldIsoToNanoseconds(startP->value.s);
  else if (startP->type == KjInt)    *startNsP = (uint64_t) startP->value.i;
  else                               return false;

  KjNode* endP = kjLookup(intervalP, "endAt");
  if (endP != NULL)
  {
    if      (endP->type == KjString) *endNsP = ldIsoToNanoseconds(endP->value.s);
    else if (endP->type == KjInt)    *endNsP = (uint64_t) endP->value.i;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// csrTemporalMatch - match the request's temporal query against a CSR
// per § 5.10.2.4.
//
// timeproperty selects the relevant interval:
//   observedAt (or default) → observationInterval
//   createdAt/modifiedAt/deletedAt → managementInterval
//
// If the relevant interval is not present, no match.
//
// Match semantics (§ 5.10.2.4):
//   - "before" / "after": timeAt is contained in or is an endpoint of the interval
//   - "between": [timeAt, endTimeAt] overlaps with [startAt, endAt]
//
// `endNs == 0` means the interval is still open (no endAt). Treated as
// "now or later" for overlap math: timeAt point matches if startNs ≤ timeAt;
// "between" overlaps as long as endTimeAt ≥ startNs.
//
static bool csrTemporalMatch(KjNode* regTree)
{
  const char* tp = swNgsild.timeproperty;
  bool isManagement = (tp != NULL && (strcmp(tp, "createdAt")  == 0 ||
                                       strcmp(tp, "modifiedAt") == 0 ||
                                       strcmp(tp, "deletedAt")  == 0));

  // The cache holds the @context-expanded tree. Look up both the short
  // name and the v1.9 expanded IRI for robustness.
  const char* shortName    = isManagement ? "managementInterval" : "observationInterval";
  const char* expandedName = isManagement ? "https://uri.etsi.org/ngsi-ld/managementInterval"
                                          : "https://uri.etsi.org/ngsi-ld/observationInterval";
  KjNode* intervalP = kjLookup(regTree, shortName);
  if (intervalP == NULL)
    intervalP = kjLookup(regTree, expandedName);

  uint64_t startNs = 0, endNs = 0;
  if (!intervalNs(intervalP, &startNs, &endNs))
    return false;  // CSR has no relevant interval — no match per spec

  if (strcmp(swNgsild.timerel, "between") == 0)
  {
    uint64_t qStart = swNgsild.timeAtNs;
    uint64_t qEnd   = swNgsild.endTimeAtNs;
    // Overlap test: max(qStart, startNs) ≤ min(qEnd, endNs); endNs=0 = open
    uint64_t lo = (qStart > startNs) ? qStart : startNs;
    if (endNs == 0)
      return lo <= qEnd;
    uint64_t hi = (qEnd < endNs) ? qEnd : endNs;
    return lo <= hi;
  }

  // before / after: timeAt within the interval (inclusive endpoints).
  uint64_t qAt = swNgsild.timeAtNs;
  if (qAt < startNs) return false;
  if (endNs == 0)    return true;   // open interval: anything ≥ startNs matches
  return qAt <= endNs;
}



bool getCsourceRegistrations(void)
{
  bool hasAttrs = (swNgsild.attrsV != NULL && swNgsild.attrsV[0] != NULL);
  bool hasPick  = (swNgsild.pickV  != NULL && swNgsild.pickV[0]  != NULL);
  bool hasQ     = (swNgsild.q      != NULL && swNgsild.q[0]      != 0);

  // § 6.8.3.2 Table 6.8.3.2-1: `attrs` is a deprecated synonym for
  // `pick + q`. Accept either, but never both at once.
  if (hasAttrs && hasPick)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Conflicting Query",
            "'attrs' and 'pick' are mutually exclusive ('attrs' is deprecated; use 'pick')");
    return true;
  }

  // Filter whitelist. 501 anything we haven't implemented.
  for (int i = 0; i < swRest.in.uriParamCount; i++)
  {
    const char* key = swRest.in.uriParamV[i].key;
    if (paramSupported(key) == false)
    {
      ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
              "Context Source Registration discovery filter '%s' is not implemented; supported: type, id, idPattern, pick, omit, attrs (deprecated), q, csf, scopeQ, lang, geometryProperty, limit, offset, count",
              key);
      return true;
    }
  }

  // § 5.10.2.4: at least one of type / attrs / pick / q / geoQ.
  bool hasType = (swNgsild.typeV != NULL && swNgsild.typeV[0] != NULL);
  if (!hasType && !hasAttrs && !hasPick && !hasQ)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Too Wide Query",
            "at least one of 'type', 'attrs', 'pick', 'q' or the geo-query quadruple is required");
    return true;
  }

  // Note: § 5.10.2 filters on the CSR's own user-Properties (organization,
  // tier, etc.), not on target entities. ldEntityMatchQ walks the NGSI-LD
  // Property shape { "type": "Property", "value": X } regardless of the
  // host object, so CSRs with user-Properties work out of the box.
  // swNgsild.qExpr is already parsed (with attrs expanded via the request
  // @context) by ldUrlParams at pre-service time — just reuse it.
  LdQNode* qExpr = hasQ ? swNgsild.qExpr : NULL;

  Tenant*     tenantP = (Tenant*) swNgsild.tenantP;
  LdRegCache* cacheP  = (LdRegCache*) tenantP->regCacheP;

  KjNode* arrayP = kjArray(swRest.kjsonP, NULL);

  if (cacheP != NULL)
  {
    const char* entityIdFilter = (swNgsild.idV != NULL && swNgsild.idV[0] != NULL)
                                 ? swNgsild.idV[0] : NULL;

    // attrs wins when supplied (deprecated but honoured); otherwise pick.
    char** attrsFilter = hasAttrs ? swNgsild.attrsV : (hasPick ? swNgsild.pickV : NULL);

    LdRegCacheItem** matchV = NULL;
    int matchN = ldRegCacheMatchForDiscovery(cacheP, swNgsild.typeV,
                                              entityIdFilter, swNgsild.idPattern,
                                              attrsFilter, &matchV);

    // Post-filters compact matchV in-place. Order: q (CSR user-Properties)
    // then scopeQ (CSR's scope field). § 5.10.2.4 lists them as separate
    // conjunctive conditions on the regs.
    int passN = matchN;

    if (qExpr != NULL)
    {
      int n = 0;
      for (int i = 0; i < passN; i++)
      {
        if (matchV[i]->regTree != NULL && ldEntityMatchQ(matchV[i]->regTree, qExpr))
          matchV[n++] = matchV[i];
      }
      passN = n;
    }

    if (swNgsild.scopeExpr != NULL)
    {
      int n = 0;
      for (int i = 0; i < passN; i++)
      {
        KjNode* scopeP = (matchV[i]->regTree != NULL) ? kjLookup(matchV[i]->regTree, "scope") : NULL;
        if (ldEntityMatchScope(scopeP, swNgsild.scopeExpr))
          matchV[n++] = matchV[i];
      }
      passN = n;
    }

    // § 5.10.2.4 temporal query:
    //   - When timerel is absent: only CSRs WITHOUT intervals (latest-info
    //     sources) are kept.
    //   - When timerel is present: only CSRs WITH the relevant interval
    //     (per timeproperty) AND that satisfy the temporal predicate.
    {
      int n = 0;
      for (int i = 0; i < passN; i++)
      {
        KjNode* tree = matchV[i]->regTree;
        if (tree == NULL)
          continue;

        bool hasObs  = (kjLookup(tree, "observationInterval") != NULL ||
                        kjLookup(tree, "https://uri.etsi.org/ngsi-ld/observationInterval") != NULL);
        bool hasMgmt = (kjLookup(tree, "managementInterval")  != NULL ||
                        kjLookup(tree, "https://uri.etsi.org/ngsi-ld/managementInterval")  != NULL);

        if (swNgsild.timerel == NULL)
        {
          // No temporal query → keep only "latest-info" CSRs (no intervals).
          if (!hasObs && !hasMgmt)
            matchV[n++] = matchV[i];
        }
        else
        {
          // Temporal query present → keep only CSRs with the relevant
          // interval AND that match the predicate.
          if (csrTemporalMatch(tree))
            matchV[n++] = matchV[i];
        }
      }
      passN = n;
    }

    // § 5.10.2.4 geoQ: filter by overlap with the CSR's geo-coverage field
    // (location / observationSpace / operationSpace) selected by the
    // request's geoproperty (default: location).
    if (swNgsild.geoRel != NULL && cacheP->csrGeoMatchFunc != NULL)
    {
      const char* prop = swNgsild.geoproperty;
      bool isObs = (prop != NULL) && (strcmp(prop, "observationSpace") == 0 ||
                                      strcmp(prop, "https://uri.etsi.org/ngsi-ld/observationSpace") == 0);
      bool isOp  = (prop != NULL) && (strcmp(prop, "operationSpace") == 0 ||
                                      strcmp(prop, "https://uri.etsi.org/ngsi-ld/operationSpace") == 0);

      int n = 0;
      for (int i = 0; i < passN; i++)
      {
        KjNode* csrGeoP = isObs ? matchV[i]->observationSpaceP
                          : isOp ? matchV[i]->operationSpaceP
                          : matchV[i]->locationP;
        if (cacheP->csrGeoMatchFunc(csrGeoP, swNgsild.geoRel, swNgsild.geometry, swNgsild.coordinates))
          matchV[n++] = matchV[i];
      }
      passN = n;
    }

    int skip  = (swNgsild.offset > 0) ? swNgsild.offset : 0;
    int limit = (swNgsild.limit  > 0) ? swNgsild.limit  : passN;

    // § 5.10.2.5 — return filtered RegistrationInfo. Compile the request
    // idPattern once for reuse across all matching items.
    regex_t  idRegex;
    bool     haveIdRegex = false;
    if (swNgsild.idPattern != NULL && swNgsild.idPattern[0] != 0)
    {
      if (regcomp(&idRegex, swNgsild.idPattern, REG_EXTENDED | REG_NOSUB) == 0)
        haveIdRegex = true;
    }

    char** attrsFilterRsp = hasAttrs ? swNgsild.attrsV : (hasPick ? swNgsild.pickV : NULL);
    const char* idFilter  = (swNgsild.idV != NULL && swNgsild.idV[0] != NULL)
                             ? swNgsild.idV[0] : NULL;

    for (int i = skip; i < passN && (i - skip) < limit; i++)
    {
      if (matchV[i]->regTree != NULL)
      {
        KjNode* clone = kjClone(swRest.kjsonP, matchV[i]->regTree);

        // § 5.10.2.5 — strip every information[] entry that doesn't match the request's discovery filter. The pre-parsed
        // infoV linked list is in lockstep with the regTree's "information" array, so we iterate both in parallel and unlink
        // the non-matching JSON nodes. § 5.10.2.5 wording is "should", not "shall"; --testConformance flips the broker into
        // ETSI mode (keep the full information[] of every matched CSR) — see spec-doubts § 26 / testsuite-doubts § 25.
        if (!ldTestConformance)
        {
          KjNode* infoP = kjLookup(clone, "information");
          if (infoP != NULL && infoP->type == KjArray)
          {
            KjNode*    childP = infoP->value.firstChildP;
            LdRegInfo* riP    = matchV[i]->infoV;
            while (childP != NULL && riP != NULL)
            {
              KjNode*    nextChild = childP->next;
              LdRegInfo* nextRi    = riP->next;
              if (!ldRegInfoDiscoveryMatches(riP, idFilter, swNgsild.typeV,
                                              haveIdRegex ? &idRegex : NULL,
                                              attrsFilterRsp))
                kjChildRemove(infoP, childP);
              childP = nextChild;
              riP    = nextRi;
            }
          }
        }

        // omit strips listed members from the response (mirror of pick,
        // applied on the OUTPUT side only — pick narrows reg matching;
        // omit just trims the response).
        if (swNgsild.omitV != NULL && swNgsild.omitV[0] != NULL)
          ldPickOmit(clone, NULL, swNgsild.omitV);

        // lang reduces LanguageProperty attrs on user-Properties.
        if (swNgsild.lang != NULL)
          ldLangReduce(clone, swNgsild.lang, &swRest.kalloc);

        // --testConformance: wrap CSR geo-coverage fields as GeoProperty on this list endpoint. § 5.2.9 says these are
        // bare GeoJSON on the wire; ETSI 037_07 / 037_05 / 037_08 fixtures expect the normalized GeoProperty wrapper on
        // discovery responses (the single-CSR retrieve endpoint must keep bare per ETSI 033_01_03). See spec-doubts § 26.
        if (ldTestConformance)
        {
          static const char* geoFields[] = { "location", "observationSpace", "operationSpace" };
          for (size_t gf = 0; gf < sizeof(geoFields) / sizeof(geoFields[0]); gf++)
          {
            KjNode* geoP = kjLookup(clone, geoFields[gf]);
            if (geoP == NULL || geoP->type != KjObject) continue;
            KjNode* tNode = kjLookup(geoP, "type");
            if (tNode == NULL || tNode->type != KjString) continue;
            if (strcmp(tNode->value.s, "GeoProperty") == 0) continue;   // already wrapped
            ldWrapAsGeoProperty(clone, geoP, &swRest.kalloc);
          }
        }

        kjChildAdd(arrayP, clone);
      }
    }

    if (haveIdRegex) regfree(&idRegex);
    if (matchV != NULL) free(matchV);

    // § 6.3.10 Link header for pagination — rel=next when more rows
    // remain past skip+limit, rel=prev/first when offset > 0.
    bool hasMore = (skip + limit < passN);
    if (hasMore || skip > 0)
      ldPaginationLinkHeader(hasMore);

    // § 6.3.5 NGSILD-Results-Count header when ?count=true.
    if (swNgsild.count)
    {
      char* countStr = (char*) kaAlloc(&swRest.kalloc, 32);
      snprintf(countStr, 32, "%d", passN);
      swRestOutHeaderAdd("NGSILD-Results-Count", countStr);
    }
  }

  ldContextResolve();

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = arrayP;
  return true;
}

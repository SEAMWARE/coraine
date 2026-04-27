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
#include <stdlib.h>                                  // free
#include <string.h>                                  // strcmp

#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjChildAdd
#include "kjson/kjClone.h"                           // kjClone
#include "kjson/kjLookup.h"                          // kjLookup

#include "swRest/SwRestState.h"                      // swRest
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldContextResolve
#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForDiscovery
#include "swNgsild/ldEntityMatch.h"                  // ldEntityMatchQ, ldEntityMatchScope
#include "swNgsild/ldPickOmit.h"                     // ldPickOmit
#include "swNgsild/ldLangReduce.h"                   // ldLangReduce

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
  return false;
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
      ldError(501, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
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
  LdRegCache* cacheP  = (tenantP != NULL) ? (LdRegCache*) tenantP->regCacheP : NULL;

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

    for (int i = skip; i < passN && (i - skip) < limit; i++)
    {
      if (matchV[i]->regTree != NULL)
      {
        KjNode* clone = kjClone(swRest.kjsonP, matchV[i]->regTree);

        // omit strips listed members from the response (mirror of pick,
        // applied on the OUTPUT side only — pick narrows reg matching;
        // omit just trims the response).
        if (swNgsild.omitV != NULL && swNgsild.omitV[0] != NULL)
          ldPickOmit(clone, NULL, swNgsild.omitV);

        // lang reduces LanguageProperty attrs on user-Properties.
        if (swNgsild.lang != NULL)
          ldLangReduce(clone, swNgsild.lang, &swRest.kalloc);

        kjChildAdd(arrayP, clone);
      }
    }

    if (matchV != NULL) free(matchV);
  }

  ldContextResolve();

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = arrayP;
  return true;
}

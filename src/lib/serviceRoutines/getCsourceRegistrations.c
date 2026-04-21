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
// Deferred (501 when supplied alone): q, geoQ, csf, scopeQ, timerel,
// lang, omit, geometryProperty. When q is supplied alongside attrs
// or pick we proceed using attrs/pick — the value-filter role of q
// is a separate future concern.
//
// Response: 200 OK + JSON-LD array of CSourceRegistration (§ 5.10.2.5).
//
#include <stddef.h>                                  // NULL
#include <stdlib.h>                                  // free
#include <string.h>                                  // strcmp

#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjChildAdd
#include "kjson/kjClone.h"                           // kjClone

#include "swRest/SwRestState.h"                      // swRest
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldContextResolve
#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForDiscovery

#include "db/DbDriver.h"                             // db, DB_OK
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getCsourceRegistrations.h" // Own interface



static bool paramSupported(const char* key)
{
  if (key == NULL)                       return true;
  if (strcmp(key, "limit")     == 0)     return true;
  if (strcmp(key, "offset")    == 0)     return true;
  if (strcmp(key, "count")     == 0)     return true;
  if (strcmp(key, "type")      == 0)     return true;
  if (strcmp(key, "id")        == 0)     return true;
  if (strcmp(key, "idPattern") == 0)     return true;
  if (strcmp(key, "pick")      == 0)     return true;
  if (strcmp(key, "attrs")     == 0)     return true;
  if (strcmp(key, "q")         == 0)     return true;   // handled separately (501 alone, tolerated with attrs/pick)
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

  // Filter whitelist. 501 anything we haven't implemented — EXCEPT `q` if
  // it arrives alongside attrs/pick (use attrs/pick as the entity
  // selector and treat the q-half of the synonym as noise).
  for (int i = 0; i < swRest.in.uriParamCount; i++)
  {
    const char* key = swRest.in.uriParamV[i].key;
    if (key != NULL && strcmp(key, "q") == 0 && (hasAttrs || hasPick))
      continue;
    if (paramSupported(key) == false)
    {
      ldError(501, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
              "Context Source Registration discovery filter '%s' is not implemented; supported: type, id, idPattern, pick, attrs (deprecated), limit, offset, count",
              key);
      return true;
    }
  }

  // q on its own is still 501.
  if (hasQ && !hasAttrs && !hasPick)
  {
    ldError(501, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
            "Context Source Registration discovery with 'q' as sole filter is not implemented");
    return true;
  }

  // § 5.10.2.4: at least one of type / attrs / q / geoQ. In v1 type or
  // an attribute-name list (pick or attrs) satisfies that.
  bool hasType = (swNgsild.typeV != NULL && swNgsild.typeV[0] != NULL);
  if (!hasType && !hasAttrs && !hasPick)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Too Wide Query",
            "at least one of 'type', 'attrs', 'pick', 'q' or the geo-query quadruple is required");
    return true;
  }

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

    int skip  = (swNgsild.offset > 0) ? swNgsild.offset : 0;
    int limit = (swNgsild.limit  > 0) ? swNgsild.limit  : matchN;

    for (int i = skip; i < matchN && (i - skip) < limit; i++)
    {
      if (matchV[i]->regTree != NULL)
        kjChildAdd(arrayP, kjClone(swRest.kjsonP, matchV[i]->regTree));
    }

    if (matchV != NULL) free(matchV);
  }

  ldContextResolve();

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = arrayP;
  return true;
}

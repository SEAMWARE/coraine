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
// Matching runs against each CSR's pre-parsed `information[]` entries.
// § 5.10.2.4 requires at least one of: type selector, attrs, q, geoQ.
// v1 supports type + id + idPattern filters; attrs / q / geoQ / csf /
// scopeQ / timerel return 501 Not Implemented if supplied.
//
// Response: 200 OK + JSON-LD array of CSourceRegistration (§ 5.10.2.5).
// NOT a BatchOperationResult. Each matching CSR is returned whole; the
// spec allows (SHOULD) filtering the RegistrationInfo within each CSR
// but that refinement is deferred.
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



// -----------------------------------------------------------------------------
//
// paramSupported - true for the URL params this route honors in v1.
//
static bool paramSupported(const char* key)
{
  if (key == NULL)                       return true;  // ignore malformed
  if (strcmp(key, "limit")     == 0)     return true;
  if (strcmp(key, "offset")    == 0)     return true;
  if (strcmp(key, "count")     == 0)     return true;
  if (strcmp(key, "type")      == 0)     return true;
  if (strcmp(key, "id")        == 0)     return true;
  if (strcmp(key, "idPattern") == 0)     return true;
  return false;
}



// -----------------------------------------------------------------------------
//
// getCsourceRegistrations -
//
bool getCsourceRegistrations(void)
{
  //
  // 501 for any filter param we don't honor yet.
  //
  for (int i = 0; i < swRest.in.uriParamCount; i++)
  {
    const char* key = swRest.in.uriParamV[i].key;
    if (paramSupported(key) == false)
    {
      ldError(501, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
              "Context Source Registration discovery filter '%s' is not implemented; only type/id/idPattern/limit/offset/count are honored",
              key);
      return true;
    }
  }

  //
  // § 5.10.2.4: at least one of type / attrs / q / geoQ. In v1 only
  // type satisfies that.
  //
  bool hasType = (swNgsild.typeV != NULL && swNgsild.typeV[0] != NULL);
  if (!hasType)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Too Wide Query",
            "at least one of 'type', 'attrs', 'q' or the geo-query quadruple is required");
    return true;
  }

  Tenant*     tenantP = (Tenant*) swNgsild.tenantP;
  LdRegCache* cacheP  = (tenantP != NULL) ? (LdRegCache*) tenantP->regCacheP : NULL;

  KjNode* arrayP = kjArray(swRest.kjsonP, NULL);

  if (cacheP != NULL)
  {
    const char* entityIdFilter = (swNgsild.idV != NULL && swNgsild.idV[0] != NULL)
                                 ? swNgsild.idV[0] : NULL;

    LdRegCacheItem** matchV = NULL;
    int matchN = ldRegCacheMatchForDiscovery(cacheP, swNgsild.typeV,
                                              entityIdFilter, swNgsild.idPattern,
                                              &matchV);

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

//
// FILE            getCsourceRegistration.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/csourceRegistrations/{registrationId}  (NGSI-LD § 5.10.1)
//

#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/kjClone.h"                           // kjClone

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldContextResolve
#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem
#include "swNgsild/ldRegCache.h"                     // ldRegCacheItemLookup

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getCsourceRegistration.h"  // Own interface



// -----------------------------------------------------------------------------
//
// getCsourceRegistration -
//
// Served from the in-memory reg cache (companion to the list endpoint,
// which already does the same). Cloning into the request arena so the
// response path can compact without mutating cache.
//
bool getCsourceRegistration(void)
{
  const char* regId = swRest.in.wildcard[0];

  Tenant*     tenantP = (Tenant*) swNgsild.tenantP;
  LdRegCache* cacheP  = (tenantP != NULL) ? (LdRegCache*) tenantP->regCacheP : NULL;

  LdRegCacheItem* itemP = (cacheP != NULL) ? ldRegCacheItemLookup(cacheP, regId) : NULL;

  if (itemP == NULL || itemP->regTree == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "registration '%s' not found", regId);
    return true;
  }

  ldContextResolve();

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = kjClone(swRest.kjsonP, itemP->regTree);
  return true;
}

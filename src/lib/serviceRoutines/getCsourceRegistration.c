//
// FILE            getCsourceRegistration.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/csourceRegistrations/{registrationId}  (NGSI-LD § 5.10.1)
//

#include <stddef.h>                                  // NULL

#include "corRest/CorRestState.h"                      // corRest
#include "kjson/kjClone.h"                           // kjClone

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild, ldContextResolve
#include "corNgsild/ldStripSysAttrs.h"                // ldStripSysAttrs
#include "corNgsild/ldSysTimestamp.h"                 // ldSysTimestampsToIso
#include "corNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem
#include "corNgsild/ldRegCache.h"                     // ldRegCacheItemLookup

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
  const char* regId = corRest.in.wildcard[0];

  Tenant*     tenantP = (Tenant*) corNgsild.tenantP;
  LdRegCache* cacheP  = (LdRegCache*) tenantP->regCacheP;

  LdRegCacheItem* itemP = (cacheP != NULL) ? ldRegCacheItemLookup(cacheP, regId) : NULL;

  if (itemP == NULL || itemP->regTree == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "registration '%s' not found", regId);
    return true;
  }

  ldContextResolve();

  KjNode* regP = kjClone(corRest.kjsonP, itemP->regTree);

  // § 6.4.5 — createdAt/modifiedAt (nanosecond integers) → ISO 8601 under sysAttrs; stripped otherwise.
  if (corNgsild.sysAttrs == false)
    ldStripSysAttrs(regP);
  else
    ldSysTimestampsToIso(regP, &corRest.kalloc);

  corNgsild.rawResponse    = true;
  corRest.out.responseTree = regP;
  return true;
}

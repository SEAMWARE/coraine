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
#include "swNgsild/ldStripSysAttrs.h"                // ldStripSysAttrs
#include "swNgsild/ldSysTimestamp.h"                 // ldSysTimestampsToIso
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
  LdRegCache* cacheP  = (LdRegCache*) tenantP->regCacheP;

  LdRegCacheItem* itemP = (cacheP != NULL) ? ldRegCacheItemLookup(cacheP, regId) : NULL;

  if (itemP == NULL || itemP->regTree == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "registration '%s' not found", regId);
    return true;
  }

  ldContextResolve();

  KjNode* regP = kjClone(swRest.kjsonP, itemP->regTree);

  // § 6.4.5 — createdAt/modifiedAt (nanosecond integers) → ISO 8601 under sysAttrs; stripped otherwise.
  if (swNgsild.sysAttrs == false)
    ldStripSysAttrs(regP);
  else
    ldSysTimestampsToIso(regP, &swRest.kalloc);

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = regP;
  return true;
}

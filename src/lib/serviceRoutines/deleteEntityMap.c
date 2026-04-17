//
// FILE            deleteEntityMap.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// DELETE /ngsi-ld/v1/entityMaps/{entityMapId}  (NGSI-LD § 5.14.3)
//

#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdEntityMap.h"                    // LdEntityMapStore
#include "swNgsild/ldEntityMap.h"                    // ldEntityMapRemove

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/deleteEntityMap.h"         // Own interface



// -----------------------------------------------------------------------------
//
// deleteEntityMap -
//
bool deleteEntityMap(void)
{
  const char* mapId = swRest.in.wildcard[0];
  Tenant*     tP    = (Tenant*) swNgsild.tenantP;

  if (tP == NULL || tP->entityMapStoreP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity map '%s' not found", mapId);
    return true;
  }

  bool removed = ldEntityMapRemove((LdEntityMapStore*) tP->entityMapStoreP, mapId);
  if (!removed)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity map '%s' not found", mapId);
    return true;
  }

  swRest.out.httpStatusCode = 204;
  return true;
}

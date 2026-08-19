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

#include "corRest/CorRestState.h"                      // corRest
#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdEntityMap.h"                    // LdEntityMapStore
#include "corNgsild/ldEntityMap.h"                    // ldEntityMapRemove

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/deleteEntityMap.h"         // Own interface



// -----------------------------------------------------------------------------
//
// deleteEntityMap -
//
bool deleteEntityMap(void)
{
  const char* mapId = corRest.in.wildcard[0];
  Tenant*     tP    = (Tenant*) corNgsild.tenantP;

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

  corRest.out.httpStatusCode = 204;
  return true;
}

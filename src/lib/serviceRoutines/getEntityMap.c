//
// FILE            getEntityMap.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/entityMaps/{entityMapId}  (NGSI-LD § 5.14.1)
//

#include <stddef.h>                                  // NULL

#include "corRest/CorRestState.h"                      // corRest
#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdEntityMap.h"                    // LdEntityMapStore, LdEntityMap
#include "corNgsild/ldEntityMap.h"                    // ldEntityMapLookup, ldEntityMapToTree

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getEntityMap.h"            // Own interface



// -----------------------------------------------------------------------------
//
// getEntityMap -
//
bool getEntityMap(void)
{
  const char* mapId = corRest.in.wildcard[0];
  Tenant*     tP    = (Tenant*) corNgsild.tenantP;

  if (tP == NULL || tP->entityMapStoreP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity map '%s' not found", mapId);
    return true;
  }

  LdEntityMap* mapP = ldEntityMapLookup((LdEntityMapStore*) tP->entityMapStoreP, mapId);
  if (mapP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity map '%s' not found", mapId);
    return true;
  }

  KjNode* treeP = ldEntityMapToTree(mapP);
  if (treeP == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "failed to render entity map");
    return true;
  }

  corNgsild.rawResponse    = true;
  corRest.out.responseTree = treeP;
  return true;
}

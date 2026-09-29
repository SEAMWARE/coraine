//
// FILE            getEntityMap.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
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

  // Pinned while it is rendered - a DELETE or the expiry purge of another request frees it
  LdEntityMap* mapP = ldEntityMapLookupPinned((LdEntityMapStore*) tP->entityMapStoreP, mapId);
  if (mapP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity map '%s' not found", mapId);
    return true;
  }

  CorNode* treeP = ldEntityMapToTree(mapP);
  ldEntityMapUnpin(mapP);
  if (treeP == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "failed to render entity map");
    return true;
  }

  corNgsild.rawResponse    = true;
  corRest.out.responseTree = treeP;
  return true;
}

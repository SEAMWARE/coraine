//
// FILE            getAssetChannel.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/assetChannels/{assetChannelId} - one AssetChannel of the request's tenant
//
#include <stddef.h>                                   // NULL

#include "corRest/CorRestState.h"                     // corRest
#include "corNgsild/corNgsild.h"                      // corNgsild, ldError, LD_ERROR_*

#include "db/Tenant.h"                                // Tenant
#include "bridge/AssetChannel.h"                      // AssetChannel
#include "bridge/assetChannelCache.h"                 // assetChannelLookupById
#include "bridge/assetChannelRender.h"                // assetChannelRender
#include "serviceRoutines/getAssetChannel.h"          // Own interface



// -----------------------------------------------------------------------------
//
// getAssetChannel -
//
bool getAssetChannel(void)
{
  const char*  id  = corRest.in.wildcard[0];
  AssetChannel* acP = assetChannelLookupById(id, (Tenant*) corNgsild.tenantP);

  corNgsild.rawResponse = true;                       // not an Entity

  if (acP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "AssetChannel '%s' not found", id);
    return true;
  }

  corRest.out.responseTree = assetChannelRender(acP);
  return true;
}

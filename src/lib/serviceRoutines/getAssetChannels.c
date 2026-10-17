//
// FILE            getAssetChannels.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/assetChannels - the AssetChannels of the request's tenant, as loaded from the configuration
// file (read-only)
//
#include <stddef.h>                                   // NULL

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeArray, corTreeChildAdd
#include "corRest/CorRestState.h"                     // corRest
#include "corNgsild/corNgsild.h"                      // corNgsild

#include "db/Tenant.h"                                // Tenant
#include "bridge/AssetChannel.h"                      // AssetChannel
#include "bridge/assetChannelCache.h"                 // assetChannelFirst
#include "bridge/assetChannelRender.h"                // assetChannelRender
#include "serviceRoutines/getAssetChannels.h"         // Own interface



// -----------------------------------------------------------------------------
//
// getAssetChannels -
//
bool getAssetChannels(void)
{
  Tenant*  tenantP = (Tenant*) corNgsild.tenantP;
  CorNode* arrayP  = corTreeArray(corRest.kallocP, NULL);

  corNgsild.rawResponse = true;                       // not an Entity

  for (AssetChannel* acP = assetChannelFirst(); acP != NULL; acP = acP->next)
  {
    if (acP->tenantP == tenantP)
      corTreeChildAdd(arrayP, assetChannelRender(acP));
  }

  corRest.out.responseTree = arrayP;
  return true;
}

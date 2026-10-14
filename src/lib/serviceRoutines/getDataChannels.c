//
// FILE            getDataChannels.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/dataChannels - the DataChannels of the request's tenant, as loaded from the configuration
// file (read-only)
//
#include <stddef.h>                                   // NULL

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeArray, corTreeChildAdd
#include "corRest/CorRestState.h"                     // corRest
#include "corNgsild/corNgsild.h"                      // corNgsild

#include "db/Tenant.h"                                // Tenant
#include "bridge/DataChannel.h"                       // DataChannel
#include "bridge/dataChannelCache.h"                  // dataChannelFirst
#include "bridge/dataChannelRender.h"                 // dataChannelRender
#include "serviceRoutines/getDataChannels.h"          // Own interface



// -----------------------------------------------------------------------------
//
// getDataChannels -
//
bool getDataChannels(void)
{
  Tenant*  tenantP = (Tenant*) corNgsild.tenantP;
  CorNode* arrayP  = corTreeArray(corRest.kallocP, NULL);

  corNgsild.rawResponse = true;                       // not an Entity

  for (DataChannel* dcP = dataChannelFirst(); dcP != NULL; dcP = dcP->next)
  {
    if (dcP->tenantP == tenantP)
      corTreeChildAdd(arrayP, dataChannelRender(dcP));
  }

  corRest.out.responseTree = arrayP;
  return true;
}

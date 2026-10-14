//
// FILE            getDataChannel.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/dataChannels/{dataChannelId} - one DataChannel of the request's tenant
//
#include <stddef.h>                                   // NULL

#include "corRest/CorRestState.h"                     // corRest
#include "corNgsild/corNgsild.h"                      // corNgsild, ldError, LD_ERROR_*

#include "db/Tenant.h"                                // Tenant
#include "bridge/DataChannel.h"                       // DataChannel
#include "bridge/dataChannelCache.h"                  // dataChannelLookupById
#include "bridge/dataChannelRender.h"                 // dataChannelRender
#include "serviceRoutines/getDataChannel.h"           // Own interface



// -----------------------------------------------------------------------------
//
// getDataChannel -
//
bool getDataChannel(void)
{
  const char*  id  = corRest.in.wildcard[0];
  DataChannel* dcP = dataChannelLookupById(id, (Tenant*) corNgsild.tenantP);

  corNgsild.rawResponse = true;                       // not an Entity

  if (dcP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "DataChannel '%s' not found", id);
    return true;
  }

  corRest.out.responseTree = dataChannelRender(dcP);
  return true;
}

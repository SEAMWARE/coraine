//
// FILE            adminBridgeInventory.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                              // bool
#include <stdlib.h>                               // atoi, free
#include <string.h>                               // strcmp

#include "corAlloc/corAllocStrdup.h"              // corAllocStrdup
#include "corJson/corJsonParse.h"                 // corJsonParse
#include "corTree/CorNode.h"                      // CorNode
#include "corRest/CorRestState.h"                 // corRest
#include "corNgsild/corNgsild.h"                  // ldError, LD_ERROR_*
#include "corBridge/BridgeDriver.h"               // bridges, bridgeCount, BridgeDriver
#include "corBridge/BridgeBroker.h"               // BRIDGE_*

#include "api/admin/adminBridgeInventory.h"       // Own interface



// -----------------------------------------------------------------------------
//
// param - a URL parameter, or NULL
//
static const char* param(const char* key)
{
  for (int ix = 0; ix < corRest.in.uriParamCount; ix++)
  {
    if ((corRest.in.uriParamV[ix].key != NULL) && (strcmp(corRest.in.uriParamV[ix].key, key) == 0))
      return corRest.in.uriParamV[ix].value;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// adminGetBridgeInventory -
//
bool adminGetBridgeInventory(void)
{
  const char*   alias   = corRest.in.wildcard[0];
  BridgeDriver* driverP = NULL;

  for (int ix = 0; ix < bridgeCount; ix++)
  {
    if ((bridges[ix].alias != NULL) && (strcmp(bridges[ix].alias, alias) == 0))
      driverP = &bridges[ix];
  }

  if (driverP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "no bridge '%s' is loaded", alias);
    return true;
  }

  if ((driverP->abiVersion < 14) || (driverP->inventory == NULL))
  {
    ldError(501, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented", "bridge '%s' has no inventory (bridge ABI 14)", alias);
    return true;
  }

  const char* depthText = param("depth");
  int         depth     = (depthText != NULL) ? atoi(depthText) : 1;

  if ((depthText != NULL) && ((depth < 0) || (depth > 16)))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "depth is a number from 0 to 16 (a bridge may cap it lower)");
    return true;
  }

  char  err[256] = "";
  char* json     = NULL;
  int   r        = driverP->inventory(param("from"), depth, 10000, &json, err, sizeof(err));

  if (r == BRIDGE_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "bridge '%s': %s", alias, (err[0] != 0) ? err : "no such 'from'");
    return true;
  }

  if (r != BRIDGE_OK)
  {
    ldError(503, LD_ERROR_INTERNAL_ERROR, "Service Unavailable", "bridge '%s': %s", alias, (err[0] != 0) ? err : "its transport cannot answer");
    free(json);
    return true;
  }

  //
  // The bridge's JSON, parsed back into the response tree (a copy in the request's arena: the parser writes in
  // its buffer)
  //
  char*    text  = (json != NULL) ? corAllocStrdup(&corRest.kalloc, json) : NULL;
  CorNode* treeP = (text != NULL) ? corJsonParse(corRest.corJsonP, text) : NULL;

  free(json);

  if (treeP == NULL)
  {
    ldError(503, LD_ERROR_INTERNAL_ERROR, "Service Unavailable", "bridge '%s': its inventory is not JSON", alias);
    return true;
  }

  corRest.out.responseTree = treeP;
  return true;
}

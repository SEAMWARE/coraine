#ifndef BRIDGE_ENTITIES_UPSERT_H_
#define BRIDGE_ENTITIES_UPSERT_H_

//
// FILE            bridgeEntitiesUpsert.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                          // CorNode



// -----------------------------------------------------------------------------
//
// bridgeEntitiesUpsert - entities a bridge made, written as ONE batch upsert; how many were written
//
// options=update: an entity that is not there is created, an attribute that is there replaced, and the
// attributes the bridge's mapping does not name left alone. Local (not forwarded), and the bridge's own write
// (corNgsild.bridgeOwnWrite - its payload check is for clients). The checks, notifications and temporal
// events of POST /ngsi-ld/v1/entityOperations/upsert, each entity on its own.
//
// On the plugin's thread, bound to the channel's tenant (bridgeThreadBind). The entities are expanded.
//
// @param what  for the log: what made them ("record on DataChannel 'D1'")
//
extern int bridgeEntitiesUpsert(CorNode* entitiesP, int entities, const char* what);

#endif  // BRIDGE_ENTITIES_UPSERT_H_

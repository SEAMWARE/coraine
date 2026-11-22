#ifndef BRIDGE_ASSETCHANNEL_SERVICES_H_
#define BRIDGE_ASSETCHANNEL_SERVICES_H_

//
// FILE            assetChannelServices.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// An AssetChannel's Operations (an AAS Operation, an OPC UA Method) as Service Registrations of the entities
// they are in - doc/asset-channels.md § 4. Compiled in with COR_FEATURE_ASSET_CHANNEL_SERVICES.
//
#include "corTree/CorNode.h"                          // CorNode

#include "bridge/AssetChannel.h"                      // AssetChannel



// -----------------------------------------------------------------------------
//
// AssetOperation - an operation node of a model, and the entity it is in
//
typedef struct AssetOperation
{
  const char*  entityId;
  const char*  entityType;                            // compact, as the mapping names it
  CorNode*     opP;                                   // { "node": "operation", "name", "endpoint", "input", "output", ... }
} AssetOperation;



// -----------------------------------------------------------------------------
//
// assetChannelServicesSync - the Service Registrations of an AssetChannel, made what its model says
//
// One registration per operation: its entity, serviceName the operation's name, the input and output
// variables as JSON Schemas, endpoint the operation node's (the bridge says where it invokes it). Its id is
// made of the AssetChannel and the operation's entity and name - the same operation, the same registration,
// a broker restart or a new model:
//
//   urn:ngsi-ld:ServiceRegistration:assetChannel:<AssetChannel, 16 hex>:<entity, 16 hex>:<operation's name>
//
// Created when not there, replaced when it changed (createdAt kept), left alone when it did not. A
// registration of this AssetChannel that the model no longer has is deleted, and so is one of an
// AssetChannel the broker no longer has (configured away). A registration a client made is never touched:
// one that offers the same service on the same entity wins, and the operation is not registered (a warning).
//
// On the plugin's thread, bound to the AssetChannel's tenant (bridgeThreadBind), after the model's entities
// were written.
//
extern void assetChannelServicesSync(AssetChannel* acP, AssetOperation* opV, int ops);

#endif  // BRIDGE_ASSETCHANNEL_SERVICES_H_

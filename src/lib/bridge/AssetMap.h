#ifndef BRIDGE_ASSETMAP_H_
#define BRIDGE_ASSETMAP_H_

//
// FILE            AssetMap.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The mapping of an ASSETCHANNEL (AssetChannel.h) - how a source model (an OPC UA server's objects, an AAS
// shell and its submodels), handed over by its bridge as a neutral node tree, becomes a graph of entities:
//
//   "assetChannels": { "<model root>": { "entities": "natural" | [ <rule>, ... ],
//                                        "attributeLayout": "attributes" | "subAttributes" | "value",
//                                        "channelInfo": [ ... ] } }
//
//   <rule>: { "from": "<node kind>", "typeDefinition": "<id>", "semanticId": "<id>", "type": "<entity type>" }
//
// Which nodes become entities is the data's owner's to say - "entities" has no default (doc/asset-channels.md
// § 6.2). Compiled in with COR_FEATURE_ASSET_CHANNELS.
//
#include <stdbool.h>                                  // bool



// -----------------------------------------------------------------------------
//
// AssetLayout - what a container that is not an entity becomes in the entity above it
//
//   Attributes     each leaf an attribute, named by its path ("Hydraulics" / "Pressure" -> hydraulicsPressure) - the
//                  default: each one queryable with q
//   SubAttributes  one attribute, named by the container, its leaves sub-attributes
//   Value          one JsonProperty, the container's leaves as the source has them
//
typedef enum AssetLayout
{
  AssetLayoutAttributes    = 0,
  AssetLayoutSubAttributes = 1,
  AssetLayoutValue         = 2
} AssetLayout;



// -----------------------------------------------------------------------------
//
// AssetRule - one kind of node that becomes an entity
//
// A node matches when its "node" is 'from' and, when the rule names them, its typeDefinition is
// 'typeDefinition' and one of its semanticIds is 'semanticId'. The first rule a node matches names its type.
//
typedef struct AssetRule
{
  char*              from;                            // the node kind: "object", "shell", "submodel", "collection", ...
  char*              typeDefinition;                  // NULL: any
  char*              semanticId;                      // NULL: any
  char*              type;                            // the entity type, as the file wrote it
  struct AssetRule*  next;
} AssetRule;



// -----------------------------------------------------------------------------
//
// AssetMap - an AssetChannel's mapping
//
// natural: the source's own units are the entities - an "object", a "shell", a "submodel" - each its
// "typeName" as entity type (else its kind, capitalized). Else 'rules'.
//
typedef struct AssetMap
{
  bool         natural;
  AssetRule*   rules;
  AssetLayout  layout;
  char*        text;                                  // "entities" as the file said it, for GET /assetChannels
} AssetMap;

#endif  // BRIDGE_ASSETMAP_H_

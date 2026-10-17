#ifndef BRIDGE_ASSETMAP_PARSE_H_
#define BRIDGE_ASSETMAP_PARSE_H_

//
// FILE            assetMap.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                          // CorNode
#include "bridge/AssetMap.h"                          // AssetMap



// -----------------------------------------------------------------------------
//
// assetMapParse - an "assetChannels" entry as an AssetMap; NULL with *whyP saying why
//
extern AssetMap* assetMapParse(CorNode* entryP, const char** whyP);



// -----------------------------------------------------------------------------
//
// assetMapFree -
//
extern void assetMapFree(AssetMap* mapP);

#endif  // BRIDGE_ASSETMAP_PARSE_H_

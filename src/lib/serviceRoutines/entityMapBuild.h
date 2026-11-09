//
// FILE            entityMapBuild.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#ifndef ENTITY_MAP_BUILD_H
#define ENTITY_MAP_BUILD_H

#include "corTree/CorNode.h"                         // CorNode
#include "corNgsild/LdEntityMap.h"                   // LdEntityMap



// -----------------------------------------------------------------------------
//
// entityMapHeader - NGSILD-EntityMap: the resource URI of the EntityMap used (TS 104-176 clause 7)
//
extern void entityMapHeader(LdEntityMap* mapP);



// -----------------------------------------------------------------------------
//
// entityMapFill - the matching set into a new map: its query's filters, and each entity's id with the
// sources that hold it (srcMap: id -> [ "@none" | registration id ]), and the sources' own maps
// (linkedMapsTracker: registration id -> remote map id)
//
extern void entityMapFill(LdEntityMap* mapP, CorNode* arrayP, CorNode* srcMap, CorNode* linkedMapsTracker);

#endif  // ENTITY_MAP_BUILD_H

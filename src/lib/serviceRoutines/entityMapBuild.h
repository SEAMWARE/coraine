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




// -----------------------------------------------------------------------------
//
// RetrieveMap - an EntityMap taking part in a retrieval of one entity (TS 104-175 § 10.4.2.4)
//
// Retrieve Entity, Retrieve Attribute and its value: ?entityMap=true asks for a map of the entity, the
// NGSILD-EntityMap request header (or ?entityMap=<id>) names one to use. A named map that is there
// says which registrations take part (its entry for the entity), one that is gone is made anew.
//
typedef struct RetrieveMap
{
  LdEntityMap*       mapP;       // the map in use, pinned for the request; NULL: none
  LdEntityMapEntry*  entryP;     // its entry for the entity: the sources to ask - NULL when it has none (none asked)
  bool               make;       // a map is made of this retrieval
  bool               remake;     // ... in place of one that is gone
  CorNode*           sourcesP;   // the sources that answered with the entity ("@none": the local copy) - for the map made
  CorNode*           linkedP;    // registration id -> the EntityMap that source used
} RetrieveMap;



// -----------------------------------------------------------------------------
//
// retrieveMapOpen - the EntityMap of a retrieval of one entity, into rmP (always set)
//
extern void retrieveMapOpen(RetrieveMap* rmP, const char* entityId);



// -----------------------------------------------------------------------------
//
// retrieveMapSource - a source that answered with the entity ("@none" for the local copy), and the map it used
//
extern void retrieveMapSource(RetrieveMap* rmP, const char* sourceId, const char* sourceMapUrl);



// -----------------------------------------------------------------------------
//
// retrieveMapClose - the retrieval succeeded: the map made (201) or the one used (200), in NGSILD-EntityMap
//
// Returns true when that failed (the ProblemDetails set).
//
extern bool retrieveMapClose(RetrieveMap* rmP, const char* entityId);

#endif  // ENTITY_MAP_BUILD_H

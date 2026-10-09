//
// FILE            getEntities.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#ifndef GET_ENTITIES_H
#define GET_ENTITIES_H

#include <stdbool.h>                              // bool



#if COR_FEATURE_AUTO_ENTITY_MAP
// -----------------------------------------------------------------------------
//
// AutoEntityMaps - which queries get an EntityMap the client did not ask for (--autoEntityMaps)
//
// A distributed query is paginated through a map - the sources are not asked for offset/limit, so
// without one there is no correct second page. A local query pages by offset/limit in the store, and
// an automatic map for it costs its first page an extra query for the ids of every match (and the
// maps' memory); it buys pages that are slices of the set as it was at the first page. The default
// is the distributed ones only (doc/installation.md#entitymaps has the measured cost).
//
typedef enum AutoEntityMaps
{
  AutoEntityMapsNone,           // none - a map only when a client asks for one
  AutoEntityMapsDistributed,    // the queries forwarded to Context Sources (the default)
  AutoEntityMapsAll             // every query of more than one page, local ones included
} AutoEntityMaps;

extern AutoEntityMaps autoEntityMaps;
#endif



// -----------------------------------------------------------------------------
//
// getEntities -
//
extern bool getEntities(void);

#endif  // GET_ENTITIES_H

#ifndef CORRAMDB_RAMDBGEOMATCH_H_
#define CORRAMDB_RAMDBGEOMATCH_H_

//
// FILE            ramdbGeoMatch.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdbool.h>

#include "kjson/KjNode.h"
#include "db/DbQueryFilter.h"



// -----------------------------------------------------------------------------
//
// ramdbGeoInit - initialize GEOS (call once at plugin init)
//
extern void ramdbGeoInit(void);



// -----------------------------------------------------------------------------
//
// ramdbGeoClose - release GEOS resources
//
extern void ramdbGeoClose(void);



// -----------------------------------------------------------------------------
//
// ramdbGeoMatch - check if an entity matches a geo-query filter
//
// Returns true if the entity matches (or no geo filter is set).
// For "near" queries, *distanceP is set to the haversine distance in meters.
//
extern bool ramdbGeoMatch(KjNode* entityP, DbQueryFilter* filterP, double* distanceP);

#endif  // CORRAMDB_RAMDBGEOMATCH_H_

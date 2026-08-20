#ifndef CORDB_CORDBGEOMATCH_H_
#define CORDB_CORDBGEOMATCH_H_

//
// FILE            corDbGeoMatch.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>

#include "kjson/KjNode.h"
#include "db/DbQueryFilter.h"



// -----------------------------------------------------------------------------
//
// corDbGeoInit - initialize GEOS (call once at plugin init)
//
extern void corDbGeoInit(void);



// -----------------------------------------------------------------------------
//
// corDbGeoClose - release GEOS resources
//
extern void corDbGeoClose(void);



// -----------------------------------------------------------------------------
//
// corDbGeoMatch - check if an entity matches a geo-query filter
//
// Returns true if the entity matches (or no geo filter is set).
// For "near" queries, *distanceP is set to the haversine distance in meters.
//
extern bool corDbGeoMatch(KjNode* entityP, DbQueryFilter* filterP, double* distanceP);

#endif  // CORDB_CORDBGEOMATCH_H_
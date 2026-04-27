#ifndef SHARED_GEOMATCH_H_
#define SHARED_GEOMATCH_H_

//
// FILE            geoMatch.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Shared GEOS-based geo matching for entity queries and subscription notifications.
// Compiled into both swRamDB and mongoc plugins.
//
#include <stdbool.h>

#include "kjson/KjNode.h"
#include "db/DbQueryFilter.h"

#include "swNgsild/LdGeoRel.h"                            // LdGeoRel

extern void geoMatchInit(void);
extern void geoMatchClose(void);
extern bool geoMatch(KjNode* entityP, DbQueryFilter* filterP, double* distanceP);

// -----------------------------------------------------------------------------
//
// csrGeoMatchOverlap - geoQ ↔ CSR geo-coverage overlap (§ 5.10.2.4)
//
// `csrGeoP` is the CSR's stored GeoJSON geometry — `location`,
// `observationSpace`, or `operationSpace`. NULL = CSR has no constraint
// for the targeted property; the helper returns true (do not filter out).
//
// Uses the conservative "possibly contains" semantic: a CSR is a candidate
// when its geometry overlaps the geoQ's reference geometry. For "near"
// with maxDistance, the CSR matches when GEOSDistance ≤ maxDistance.
//
extern bool csrGeoMatchOverlap(KjNode* csrGeoP, LdGeoRel* geoRel, const char* geometry, const char* coordinates);

#endif  // SHARED_GEOMATCH_H_

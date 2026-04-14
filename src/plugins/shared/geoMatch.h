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

extern void geoMatchInit(void);
extern void geoMatchClose(void);
extern bool geoMatch(KjNode* entityP, DbQueryFilter* filterP, double* distanceP);

#endif  // SHARED_GEOMATCH_H_

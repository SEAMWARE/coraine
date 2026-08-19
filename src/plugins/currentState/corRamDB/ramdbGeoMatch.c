//
// FILE            ramdbGeoMatch.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Thin wrappers around the shared geoMatch implementation.
//
#include "shared/geoMatch.h"
#include "currentState/corRamDB/ramdbGeoMatch.h"

void ramdbGeoInit(void)                                              { geoMatchInit(); }
void ramdbGeoClose(void)                                             { geoMatchClose(); }
bool ramdbGeoMatch(KjNode* entityP, DbQueryFilter* filterP, double* distanceP) { return geoMatch(entityP, filterP, distanceP); }

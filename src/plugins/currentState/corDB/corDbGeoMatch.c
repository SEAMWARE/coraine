//
// FILE            corDbGeoMatch.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Thin wrappers around the shared geoMatch implementation.
//
#include "shared/geoMatch.h"
#include "currentState/corDB/corDbGeoMatch.h"

void corDbGeoInit(void)                                              { geoMatchInit(); }
void corDbGeoClose(void)                                             { geoMatchClose(); }
bool corDbGeoMatch(KjNode* entityP, DbQueryFilter* filterP, double* distanceP) { return geoMatch(entityP, filterP, distanceP); }

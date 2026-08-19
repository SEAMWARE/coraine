#ifndef CORAINE_NGSILD_SERVICES_H_
#define CORAINE_NGSILD_SERVICES_H_

//
// FILE            ngsildServices.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "corRest/CorRestService.h"                    // CorRestServiceSimplified



// -----------------------------------------------------------------------------
//
// ngsildCoreServices / ngsildCoreServiceCount -
//
// Core NGSI-LD services (GET/POST entities).
// Plugins append their services to form the final flat array.
//
extern CorRestServiceSimplified  ngsildCoreServices[];
extern int                      ngsildCoreServiceCount;



// -----------------------------------------------------------------------------
//
// serviceBuild - build the combined flat service array from core + plugins
//
// Returns a malloc'd array.  *totalCountP is set to the total number of entries.
//
extern CorRestServiceSimplified* serviceBuild(int* totalCountP);

#endif  // CORAINE_NGSILD_SERVICES_H_

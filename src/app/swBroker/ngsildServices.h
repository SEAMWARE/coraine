#ifndef SWBROKER_NGSILD_SERVICES_H_
#define SWBROKER_NGSILD_SERVICES_H_

//
// FILE            ngsildServices.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "swRest/SwRestService.h"                    // SwRestServiceSimplified



// -----------------------------------------------------------------------------
//
// ngsildCoreServices / ngsildCoreServiceCount -
//
// Core NGSI-LD services (GET/POST entities).
// Plugins append their services to form the final flat array.
//
extern SwRestServiceSimplified  ngsildCoreServices[];
extern int                      ngsildCoreServiceCount;



// -----------------------------------------------------------------------------
//
// serviceBuild - build the combined flat service array from core + plugins
//
// Returns a malloc'd array.  *totalCountP is set to the total number of entries.
//
extern SwRestServiceSimplified* serviceBuild(int* totalCountP);

#endif  // SWBROKER_NGSILD_SERVICES_H_

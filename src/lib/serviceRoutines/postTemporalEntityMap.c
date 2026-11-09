//
// FILE            postTemporalEntityMap.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                 // bool

#include "serviceRoutines/getEntitiesTemporal.h"     // createTemporalEntityMap
#include "serviceRoutines/postTemporalEntityMap.h"   // Own interface
#if !COR_FEATURE_GEOQ
#include "serviceRoutines/geoNotInThisBuild.h"       // geoQueryRefused
#endif



// -----------------------------------------------------------------------------
//
// postTemporalEntityMap - POST /temporal/entityMaps: Create EntityMap for Query Temporal Evolution of
// Entities, the query a Query in the body (TS 104-176 § 11.3.3.2)
//
// The body is params already, expanded: the payload check (ldQueryBodyCheck) made them - as for
// POST /temporal/entityOperations/query.
//
bool postTemporalEntityMap(void)
{
#if !COR_FEATURE_GEOQ
  if (geoQueryRefused())                              // the body's geoQ, now in the request's georel
    return true;
#endif

  return createTemporalEntityMap();
}

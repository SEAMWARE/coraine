//
// FILE            postTemporalEntityBatchQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// POST /ngsi-ld/v1/temporal/entityOperations/query — § 5.7.4 / § 6.24.3.1.
// Mirrors the GET /temporal/entities behaviour but moves all filter
// parameters from the URL into a § 5.2.23 Query object in the body
// (with a § 5.2.21 TemporalQuery sub-object). Useful when the query
// is too big to fit in a URL.
//
// The route's payload check (ldQueryBodyCheck) validates the body and makes it the request's params, temporalQ
// flattened, expanded as the URL's are (ldQueryBodyToParams) - this routine delegates to getEntitiesTemporal.
//

#include <stddef.h>                                  // NULL

#include "corRest/CorRestState.h"                      // corRest

#include "corTree/CorNode.h"                         // CorNode

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild

#include "serviceRoutines/getEntitiesTemporal.h"     // getEntitiesTemporal
#include "serviceRoutines/postTemporalEntityBatchQuery.h"  // Own interface
#if !COR_FEATURE_GEOQ
#include "serviceRoutines/geoNotInThisBuild.h"       // geoQueryRefused, geoQRefused
#endif



bool postTemporalEntityBatchQuery(void)
{
  //
  // The body is params already, expanded: the payload check (ldQueryBodyCheck) made them
  //
#if !COR_FEATURE_GEOQ
  if (geoQueryRefused())                              // the body's geoQ, now in the request's georel
    return true;
#endif

  return getEntitiesTemporal();
}

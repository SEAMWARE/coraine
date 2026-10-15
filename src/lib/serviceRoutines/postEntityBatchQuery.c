//
// FILE            postEntityBatchQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// POST /ngsi-ld/v1/entityOperations/query — § 6.23.3.1. Mirrors the
// GET /entities behaviour (§ 5.7.2) but moves all filter parameters
// from the URL into a § 5.2.23 Query object in the body. Useful when
// the query is too big to fit in a URL or contains characters that
// would need aggressive URL-encoding.
//
// The route's payload check (pCheckQuery) validates the body and makes it the request's params, expanded
// as the URL's are (ldQueryBodyToParams) - this routine delegates to getEntities.
//

#include <stddef.h>                                  // NULL

#include "corRest/CorRestState.h"                      // corRest

#include "corTree/CorNode.h"                         // CorNode

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild

#include "serviceRoutines/getEntities.h"             // getEntities
#include "serviceRoutines/postEntityBatchQuery.h"    // Own interface
#if !COR_FEATURE_GEOQ
#include "serviceRoutines/geoNotInThisBuild.h"       // geoQueryRefused, geoQRefused
#endif



bool postEntityBatchQuery(void)
{
  //
  // The body is params already, expanded: the payload check (pCheckQuery) made them
  //
#if !COR_FEATURE_GEOQ
  if (geoQueryRefused())                              // the body's geoQ, now in the request's georel
    return true;
#endif

  return getEntities();
}

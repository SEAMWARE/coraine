//
// FILE            postEntityBatchQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/entityOperations/query — § 6.23.3.1. Mirrors the
// GET /entities behaviour (§ 5.7.2) but moves all filter parameters
// from the URL into a § 5.2.23 Query object in the body. Useful when
// the query is too big to fit in a URL or contains characters that
// would need aggressive URL-encoding.
//
// Implementation: translate the body via ldQueryBodyToParams (shared
// with POST /entityMaps) and delegate to getEntities.
//

#include <stddef.h>                                  // NULL

#include "corRest/CorRestState.h"                      // corRest

#include "kjson/KjNode.h"                            // KjNode

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/ldQueryBody.h"                    // ldQueryBodyToParams

#include "serviceRoutines/getEntities.h"             // getEntities
#include "serviceRoutines/postEntityBatchQuery.h"    // Own interface



bool postEntityBatchQuery(void)
{
  KjNode* bodyP = corRest.in.requestTree;

  if (!ldQueryBodyToParams(bodyP))
    return true;

  return getEntities();
}

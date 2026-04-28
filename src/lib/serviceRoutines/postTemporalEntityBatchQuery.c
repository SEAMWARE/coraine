//
// FILE            postTemporalEntityBatchQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/temporal/entityOperations/query — § 5.7.4 / § 6.24.3.1.
// Mirrors the GET /temporal/entities behaviour but moves all filter
// parameters from the URL into a § 5.2.23 Query object in the body
// (with a § 5.2.21 TemporalQuery sub-object). Useful when the query
// is too big to fit in a URL.
//
// Implementation: translate the body via ldQueryBodyToParams (the same
// translator the non-temporal entityOperations/query uses, now extended
// to flatten temporalQ) and delegate to getEntitiesTemporal.
//

#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest

#include "kjson/KjNode.h"                            // KjNode

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldQueryBody.h"                    // ldQueryBodyToParams

#include "serviceRoutines/getEntitiesTemporal.h"     // getEntitiesTemporal
#include "serviceRoutines/postTemporalEntityBatchQuery.h"  // Own interface



bool postTemporalEntityBatchQuery(void)
{
  KjNode* bodyP = swRest.in.requestTree;

  if (!ldQueryBodyToParams(bodyP))
    return true;

  return getEntitiesTemporal();
}

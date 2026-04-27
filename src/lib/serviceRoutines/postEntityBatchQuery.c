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

#include "swRest/SwRestState.h"                      // swRest

#include "kjson/KjNode.h"                            // KjNode

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldQueryBody.h"                    // ldQueryBodyToParams

#include "serviceRoutines/getEntities.h"             // getEntities
#include "serviceRoutines/postEntityBatchQuery.h"    // Own interface



bool postEntityBatchQuery(void)
{
  KjNode* bodyP = swRest.in.requestTree;

  if (swRest.in.payload != NULL && bodyP == NULL)
  {
    ldError(415, LD_ERROR_INVALID_REQUEST, "Unsupported Media Type",
            "supported Content-Types: application/json, application/ld+json");
    return true;
  }

  if (bodyP == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "no payload");
    return true;
  }

  if (!ldQueryBodyToParams(bodyP))
    return true;

  return getEntities();
}

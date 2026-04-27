//
// FILE            postEntityMap.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/entityMaps — § 5.14.4 / § 6.34.3.2. Accepts a Query
// object (§ 5.2.23). Translation to the query pipeline is shared with
// POST /entityOperations/query via ldQueryBodyToParams.
//

#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest

#include "kjson/KjNode.h"                            // KjNode

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldQueryBody.h"                    // ldQueryBodyToParams

#include "serviceRoutines/createEntityMap.h"         // createEntityMap
#include "serviceRoutines/postEntityMap.h"           // Own interface



bool postEntityMap(void)
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

  return createEntityMap();
}

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

  if (!ldQueryBodyToParams(bodyP))
    return true;

  return createEntityMap();
}

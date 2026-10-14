#ifndef SRC_LIB_SERVICEROUTINES_GEONOTINTHISBUILD_H_
#define SRC_LIB_SERVICEROUTINES_GEONOTINTHISBUILD_H_

//
// FILE            geoNotInThisBuild.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// A broker built without geo-queries (COR_FEATURE_GEOQ=OFF: no GEOS, nothing that matches a geometry)
// refuses a request that asks for one - a georel in the URL or a query body, a geoQ in a subscription or a
// Service Execution object - with the 501 corNotInThisBuild gives a compiled-out route. Answering it as if
// nothing matched would be a wrong answer that looks right.
//
// Only included where COR_FEATURE_GEOQ is off.
//
#include <stdbool.h>                                  // bool
#include <stddef.h>                                   // NULL
#include <string.h>                                   // strstr

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corNgsild/CorNgsild.h"                      // corNgsild
#include "corNgsild/ldError.h"                        // ldError

#include "serviceRoutines/corNotInThisBuild.h"       // LD_ERROR_NOT_IN_THIS_BUILD



// -----------------------------------------------------------------------------
//
// geoRefuse - the 501; false, for a caller to return
//
static inline bool geoRefuse(const char* what)
{
  ldError(501, LD_ERROR_NOT_IN_THIS_BUILD, "Not Available In This Build",
          "geo-queries (%s) are not included in this build of coraine", what);
  return false;
}



// -----------------------------------------------------------------------------
//
// geoQueryRefused - the request's georel (URL parameters, or a query body once ldQueryBodyToParams read
// it), or an ordering by distance (orderBy=<geoproperty>;dist-asc|dist-desc, orderFrom - § 7.6.2.2): true
// when it has one - the 501 is then set
//
static inline bool geoQueryRefused(void)
{
  if ((corNgsild.geoRel != NULL) || (corNgsild.geometry != NULL) || (corNgsild.coordinates != NULL))
  {
    geoRefuse("georel, geometry, coordinates");
    return true;
  }

  if ((corNgsild.orderFrom != NULL) || ((corNgsild.orderBy != NULL) && (strstr(corNgsild.orderBy, ";dist-") != NULL)))
  {
    geoRefuse("orderBy by distance, orderFrom");
    return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// geoQRefused - a body's "geoQ" (a subscription, a Service Registration, a grouped execution): true when
// it has one - the 501 is then set
//
static inline bool geoQRefused(CorNode* bodyP)
{
  if ((bodyP == NULL) || (bodyP->type != CorObject) || (corTreeLookup(bodyP, "geoQ") == NULL))
    return false;

  geoRefuse("geoQ");
  return true;
}

#endif  // SRC_LIB_SERVICEROUTINES_GEONOTINTHISBUILD_H_

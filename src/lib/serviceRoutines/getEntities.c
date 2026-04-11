//
// FILE            getEntities.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stddef.h>                                  // NULL
#include <stdio.h>                                   // snprintf

#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "swRest/SwRestState.h"                      // swRest
#include "swJsonld/swldExpand.h"                     // swldExpand
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, SwNgsild, swNgsild, ldPaginationTrim, ldPaginationLinkHeader, ldPickOmit
#include "swNgsild/ldParamsValidate.h"               // ldParamsValidate

#include "db/DbDriver.h"                             // db, DB_OK

#include "serviceRoutines/getEntities.h"             // Own interface



// -----------------------------------------------------------------------------
//
// getEntities -
//
bool getEntities(void)
{
  //
  // Early exit if paramHook already set an error (e.g. invalid georel, geometry, coordinates)
  //
  if (swRest.out.problemType != NULL)
    return true;

  //
  // Cross-parameter validation (limit=0 requires count, etc.)
  //
  if (ldParamsValidate())
    return true;

  //
  // Geo-query inter-parameter validation:
  // If any of georel/geometry/coordinates is present, all three must be present.
  // geoproperty alone (without the other three) is also an error.
  //
  bool hasGeorel      = (swNgsild.georel      != NULL);
  bool hasGeometry    = (swNgsild.geometry     != NULL);
  bool hasCoordinates = (swNgsild.coordinates  != NULL);
  bool hasGeoproperty = (swNgsild.geoproperty  != NULL);

  if (hasGeorel || hasGeometry || hasCoordinates)
  {
    if (!hasGeorel || !hasGeometry || !hasCoordinates)
    {
      char missing[128] = "";
      int  len          = 0;

      if (!hasGeorel)      len += snprintf(missing + len, sizeof(missing) - len, "georel");
      if (!hasGeometry)    len += snprintf(missing + len, sizeof(missing) - len, "%sgeometry",    len > 0 ? ", " : "");
      if (!hasCoordinates) len += snprintf(missing + len, sizeof(missing) - len, "%scoordinates", len > 0 ? ", " : "");

      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid geo-query", "incomplete geo-query: missing %s", missing);
      return true;
    }
  }
  else if (hasGeoproperty)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid geo-query", "geoproperty without georel, geometry, and coordinates");
    return true;
  }

  //
  // Build query filter from URL params
  //
  DbQueryFilter filter = {0};

  filter.idV       = swNgsild.idV;
  filter.idPattern = swNgsild.idPattern;
  filter.typeV     = swNgsild.typeV;
  filter.typeExpr  = swNgsild.typeExpr;
  filter.scopeExpr = swNgsild.scopeExpr;
  filter.qExpr     = swNgsild.qExpr;
  filter.geoRel      = swNgsild.geoRel;
  filter.geometry    = swNgsild.geometry;
  filter.coordinates = swNgsild.coordinates;
  filter.geoproperty = swNgsild.geoproperty ? swNgsild.geoproperty : swldExpand(swNgsild.contextP, "location", &swRest.kalloc, NULL, NULL);
  filter.limit     = (swNgsild.limit > 0) ? swNgsild.limit + 1 : 0;
  filter.offset   = swNgsild.offset;
  filter.count    = swNgsild.count;

  //
  // Query the database
  //
  KjNode* arrayP = NULL;
  int     r      = db.entityQuery((Tenant*) swNgsild.tenantP, &filter, &arrayP);

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error querying entities");
    return true;
  }

  //
  // Add NGSILD-Results-Count header if count was requested
  //
  if (swNgsild.count)
  {
    char* countStr = (char*) kaAlloc(&swRest.kalloc, 32);
    snprintf(countStr, 32, "%ld", (long) filter.totalCount);

    // Write directly into swRest.out.headerV
    SwRestKeyValue* hV = swRest.out.headerV;
    int ix = swRest.out.headerCount;
    hV[ix].key   = "NGSILD-Results-Count";
    hV[ix].value = countStr;
    swRest.out.headerCount++;
  }

  //
  // Pagination: trim to limit and add Link header with next/prev
  //
  bool hasMore = ldPaginationTrim(arrayP, swNgsild.limit);
  ldPaginationLinkHeader(hasMore);

  //
  // Apply pick/omit attribute projection
  //
  if (swNgsild.pickV != NULL || swNgsild.omitV != NULL)
  {
    for (KjNode* entityP = arrayP->value.firstChildP; entityP != NULL; entityP = entityP->next)
      ldPickOmit(entityP, swNgsild.pickV, swNgsild.omitV);
  }

  swRest.out.responseTree = arrayP;
  return true;
}

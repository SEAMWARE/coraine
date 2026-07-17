#ifndef DB_DBQUERYFILTER_H_
#define DB_DBQUERYFILTER_H_

//
// FILE            DbQueryFilter.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdbool.h>                                     // bool
#include <stdint.h>                                      // int64_t

#include "swNgsild/LdGeoRel.h"                           // LdGeoRel
#include "swNgsild/LdQ.h"                               // LdQNode
#include "swNgsild/LdScopeExpr.h"                        // LdScopeExpr
#include "swNgsild/LdTypeExpr.h"                         // LdTypeExpr



// -----------------------------------------------------------------------------
//
// DbQueryFilter - filter criteria for entity queries
//
typedef struct DbQueryFilter
{
  char**       idV;        // NULL-terminated array of entity IDs (or NULL = no filter)
  char*        idPattern;  // regex pattern for entity ID matching (or NULL = no filter)
  char**       typeV;      // NULL-terminated array of expanded type URIs (simple OR only)
  LdTypeExpr*  typeExpr;   // parsed type selection expression (OR-of-AND, or NULL)

  LdScopeExpr* scopeExpr;  // parsed scopeQ expression (OR-of-AND, or NULL)
  LdQNode*     qExpr;     // parsed q expression tree (or NULL)

  // Geo-query
  LdGeoRel*  geoRel;       // parsed georel (or NULL = no geo filter)
  char*      geometry;     // reference geometry type ("Point", "Polygon", ...)
  char*      coordinates;  // reference geometry coordinates (JSON array string)
  char*      geoproperty;  // expanded IRI of the geoproperty to query

  // Sort-by-distance (§ 7.6.2.2, orderBy=<geoprop>;dist-asc|dist-desc) — orthogonal
  // to the geoRel filter. When distGeoproperty != NULL the plugin ranks entities by
  // distance from distFrom to that GeoProperty (nearest first, or farthest if
  // distDesc), attaching each entity's `geoDistance`; entities lacking the
  // GeoProperty rank last. distFrom is always a Point (non-Point → 501 upstream).
  char*      distGeoproperty;  // expanded IRI of the GeoProperty to sort by (NULL = no dist-sort)
  char*      distFrom;         // reference Point coordinates (JSON array string)
  bool       distDesc;         // true → farthest first (dist-desc)

  int     limit;      // max entities to return
  int     offset;     // entities to skip
  bool    count;      // whether to compute total count
  int64_t totalCount; // OUTPUT: total count (set by plugin when count==true)

  // OUTPUT: error reporting from plugin → service routine.
  // The plugin fills these in when returning DB_BAD_INPUT or DB_ERR;
  // the service routine maps them onto ldError so the caller sees a
  // useful ProblemDetails body (not the generic 500 placeholder).
  //
  // errType / errTitle MUST point at string-literal lifetime (e.g. one
  // of the LD_ERROR_* macros in swNgsild/LdProblem.h) — ldError stores
  // them as raw pointers without copying.
  int           errStatus;       // HTTP status to surface (400, 500, …)
  const char*   errType;         // ProblemDetails type URI (LD_ERROR_*)
  const char*   errTitle;        // ProblemDetails title (string literal)
  char          errDetail[512];  // human-readable detail from the storage layer
                                 // (sized to fit mongo's bson_error_t.message,
                                 // which is 504 bytes)
} DbQueryFilter;

#endif  // DB_DBQUERYFILTER_H_

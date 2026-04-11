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

  int     limit;      // max entities to return
  int     offset;     // entities to skip
  bool    count;      // whether to compute total count
  int64_t totalCount; // OUTPUT: total count (set by plugin when count==true)
} DbQueryFilter;

#endif  // DB_DBQUERYFILTER_H_

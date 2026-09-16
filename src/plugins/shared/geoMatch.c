//
// FILE            geoMatch.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <math.h>                                        // sin, cos, asin, sqrt, M_PI
#include <stdio.h>                                       // snprintf
#include <stdlib.h>                                      // strtod
#include <string.h>                                      // strcmp, strlen
#include <stdbool.h>                                     // bool

#include <geos_c.h>                                      // GEOS C API

#include "kjson/KjNode.h"                                // KjNode
#include "kjson/kjLookup.h"                              // kjLookup

#include "corNgsild/LdGeoRel.h"                           // LdGeoRel, LdGeoRelType
#include "corNgsild/LdVocab.h"                            // LD_VOCAB_*
#include "db/DbQueryFilter.h"                            // DbQueryFilter

#include "shared/geoMatch.h"                             // Own interface



// -----------------------------------------------------------------------------
//
// GEOS context handle (threadsafe)
//
static GEOSContextHandle_t geosCtx = NULL;



// -----------------------------------------------------------------------------
//
// geoMatchInit / geoMatchClose
//
void geoMatchInit(void)
{
  geosCtx = GEOS_init_r();
}

void geoMatchClose(void)
{
  if (geosCtx != NULL)
  {
    GEOS_finish_r(geosCtx);
    geosCtx = NULL;
  }
}



// -----------------------------------------------------------------------------
//
// geojsonToGeos - build a GEOS geometry from geometry type + coordinates string
//
// geometry:    "Point", "Polygon", "LineString", "MultiPolygon", etc.
// coordinates: JSON array string, e.g. "[-3.703,40.417]" or "[[[...]]]"
//
static GEOSGeometry* geojsonToGeos(const char* geometry, const char* coordinates)
{
  // Build a GeoJSON string:  {"type":"Point","coordinates":[-3.703,40.417]}
  char buf[4096];
  snprintf(buf, sizeof(buf), "{\"type\":\"%s\",\"coordinates\":%s}", geometry, coordinates);

  GEOSGeoJSONReader* reader = GEOSGeoJSONReader_create_r(geosCtx);
  if (reader == NULL)
    return NULL;

  GEOSGeometry* geom = GEOSGeoJSONReader_readGeometry_r(geosCtx, reader, buf);
  GEOSGeoJSONReader_destroy_r(geosCtx, reader);

  return geom;
}



// -----------------------------------------------------------------------------
//
// entityGeoPropGet - find the GeoProperty value node in an entity
//
// Entity layout:
//   { "id": "...", "type": "...", "location": { "@none": { "type": "GeoProperty", "value": { ... } } } }
//
// The geoproperty name arrives in the SAME form the attribute is stored under,
// which is what ldExpandParams produced for the query parameter:
//
//   location, observationSpace, operationSpace   short - they are core context
//                                                terms and core terms are NOT
//                                                expanded
//   anything user-defined                        the expanded IRI
//
// This comment used to say "the expanded IRI (e.g.
// https://uri.etsi.org/ngsi-ld/location)", which is wrong for exactly the three
// names that matter here. It cost an afternoon: a deliberate break written
// against it - fall back to kjLookup(entityP, "https://uri.etsi.org/ngsi-ld/location")
// - was inert, looked like the test failing to discriminate, and sent the
// investigation at the test instead of at the comment.
//
// Returns the "value" node of the GeoProperty (which is a GeoJSON object).
//
static KjNode* entityGeoPropGet(KjNode* entityP, const char* geoproperty)
{
  KjNode* attrP = kjLookup(entityP, geoproperty);
  if (attrP == NULL || attrP->type != KjObject)
    return NULL;

  // First child is the default instance ("@none")
  KjNode* instP = attrP->value.firstChildP;
  if (instP == NULL || instP->type != KjObject)
    return NULL;

  // Get "value" from the instance — should be a GeoJSON object
  return kjLookup(instP, "value");
}



// -----------------------------------------------------------------------------
//
// kjRenderCoords - render a KjNode coordinate array to a JSON string
//
// Returns number of characters written, or -1 on overflow.
//
static int kjRenderCoords(KjNode* nodeP, char* buf, int bufSize)
{
  int pos = 0;

  if (nodeP->type == KjArray)
  {
    if (pos < bufSize) buf[pos++] = '[';
    bool first = true;
    for (KjNode* childP = nodeP->value.firstChildP; childP != NULL; childP = childP->next)
    {
      if (!first && pos < bufSize) buf[pos++] = ',';
      first = false;
      int written = kjRenderCoords(childP, buf + pos, bufSize - pos);
      if (written < 0) return -1;
      pos += written;
    }
    if (pos < bufSize) buf[pos++] = ']';
  }
  else if (nodeP->type == KjFloat)
  {
    pos += snprintf(buf + pos, bufSize - pos, "%.15g", nodeP->value.f);
  }
  else if (nodeP->type == KjInt)
  {
    pos += snprintf(buf + pos, bufSize - pos, "%lld", (long long) nodeP->value.i);
  }

  return pos;
}



// -----------------------------------------------------------------------------
//
// entityGeoToGeos - convert an entity's GeoJSON value node to a GEOS geometry
//
static GEOSGeometry* entityGeoToGeos(KjNode* geojsonP)
{
  KjNode* typeP   = kjLookup(geojsonP, "type");
  KjNode* coordsP = kjLookup(geojsonP, "coordinates");

  if (typeP == NULL || typeP->type != KjString || coordsP == NULL)
    return NULL;

  char coordBuf[4096];
  int  pos = kjRenderCoords(coordsP, coordBuf, sizeof(coordBuf));
  if (pos <= 0 || pos >= (int) sizeof(coordBuf))
    return NULL;
  coordBuf[pos] = 0;

  return geojsonToGeos(typeP->value.s, coordBuf);
}



// -----------------------------------------------------------------------------
//
// geoEntityValidate - true if every GeoProperty value in the entity is a valid
// GEOS geometry.
//
// A degenerate or self-intersecting polygon (zero-area ring of identical points,
// figure-eight, ...) is rejected — mirroring the rejection a mongo 2dsphere
// index gives for free on insert, so the in-memory store does not silently
// accept geometry that cannot be indexed. entityP is in DB-model form
// (attr -> dataset instance -> { type: GeoProperty, value: GeoJSON }).
//
bool geoEntityValidate(KjNode* entityP)
{
  if (entityP == NULL || entityP->type != KjObject)
    return true;

  for (KjNode* attrP = entityP->value.firstChildP; attrP != NULL; attrP = attrP->next)
  {
    if (attrP->type != KjObject)
      continue;

    for (KjNode* instP = attrP->value.firstChildP; instP != NULL; instP = instP->next)
    {
      if (instP->type != KjObject)
        continue;

      KjNode* typeP = kjLookup(instP, "type");
      if (typeP == NULL || typeP->type != KjString || strcmp(typeP->value.s, "GeoProperty") != 0)
        continue;

      KjNode* geojsonP = kjLookup(instP, "value");
      if (geojsonP == NULL || geojsonP->type != KjObject)
        continue;

      GEOSGeometry* geom = entityGeoToGeos(geojsonP);
      if (geom == NULL)
        return false;  // unparseable / unbuildable geometry

      char valid = GEOSisValid_r(geosCtx, geom);
      GEOSGeom_destroy_r(geosCtx, geom);

      if (valid != 1)
        return false;
    }
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// Haversine distance (meters) between two lon/lat points
//
#define EARTH_RADIUS_M  6371000.0
#define DEG_TO_RAD      (M_PI / 180.0)

static double haversineDistance(double lon1, double lat1, double lon2, double lat2)
{
  double dLat = (lat2 - lat1) * DEG_TO_RAD;
  double dLon = (lon2 - lon1) * DEG_TO_RAD;

  double a = sin(dLat / 2) * sin(dLat / 2) +
             cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) *
             sin(dLon / 2) * sin(dLon / 2);

  return EARTH_RADIUS_M * 2.0 * asin(sqrt(a));
}



// -----------------------------------------------------------------------------
//
// geoMatch - check if an entity matches a geo-query filter
//
// -----------------------------------------------------------------------------
//
// csrDistanceMeters - metres between two geometries, or -1
//
// Two Points → exact haversine. Otherwise convert GEOS planar distance to
// metres: longitude shrinks as cos(latitude), so a flat 111320 m/° factor
// over-reports at high latitudes, which *underreports* matches near
// maxDistance. Exact for Point-Point, conservative enough for the
// non-point coverage a CSR declares.
//
// Shared by the dispatch filter and the exact matcher, which need the same
// arithmetic and disagree only about the topological relations.
//
static double csrDistanceMeters(const GEOSGeometry* refGeom, const GEOSGeometry* csrGeom)
{
  double xRef, yRef, xCsr, yCsr;

  if (GEOSGeomTypeId_r(geosCtx, refGeom) == GEOS_POINT &&
      GEOSGeomTypeId_r(geosCtx, csrGeom) == GEOS_POINT &&
      GEOSGeomGetX_r(geosCtx, refGeom, &xRef) == 1 &&
      GEOSGeomGetY_r(geosCtx, refGeom, &yRef) == 1 &&
      GEOSGeomGetX_r(geosCtx, csrGeom, &xCsr) == 1 &&
      GEOSGeomGetY_r(geosCtx, csrGeom, &yCsr) == 1)
    return haversineDistance(xRef, yRef, xCsr, yCsr);

  double distanceDegrees = -1;
  if (GEOSDistance_r(geosCtx, refGeom, csrGeom, &distanceDegrees) == 1)
    return distanceDegrees * 111320.0;

  return -1;
}



// -----------------------------------------------------------------------------
//
// geoRelEval - the topological georels of § 7.2.4, evaluated exactly
//
// Factored out of geoMatch because TWO callers need the exact answer on a pair
// of geometries: an Entity's GeoProperty against the query reference, and - for
// CSR DISCOVERY - a registration's own geo field against it. Discovery asks
// about the registration itself, so "disjoint" has to mean disjoint.
//
// Not to be confused with the dispatch filter below, which asks a different
// question and must not use this.
//
static bool geoRelEval(LdGeoRelType rel, const GEOSGeometry* refGeom, const GEOSGeometry* targetGeom)
{
  switch (rel)
  {
  case LdGeoWithin:      return (GEOSContains_r(geosCtx, refGeom, targetGeom) == 1);
  case LdGeoContains:    return (GEOSContains_r(geosCtx, targetGeom, refGeom) == 1);
  case LdGeoIntersects:  return (GEOSIntersects_r(geosCtx, refGeom, targetGeom) == 1);
  case LdGeoDisjoint:    return (GEOSDisjoint_r(geosCtx, refGeom, targetGeom) == 1);
  case LdGeoOverlaps:
    //
    // § 7.2.4: "the target geometry shall overlap, as specified by [n.21]" —
    // OGC 06-103r4 overlap, which GEOSOverlaps implements exactly: the two
    // geometries must share the same dimension, their interiors must meet,
    // and neither may contain the other. So a Point never overlaps a Polygon,
    // and a geometry never overlaps one it is within, contains or equals.
    //
    return (GEOSOverlaps_r(geosCtx, refGeom, targetGeom) == 1);
  case LdGeoEquals:      return (GEOSEquals_r(geosCtx, refGeom, targetGeom) == 1);
  default:               return false;
  }
}



bool geoMatch(KjNode* entityP, DbQueryFilter* filterP, double* distanceP)
{
  if (distanceP != NULL)
    *distanceP = -1;

  if (filterP == NULL || filterP->geoRel == NULL)
    return true;  // no geo filter

  // Find the entity's geoproperty
  KjNode* geojsonP = entityGeoPropGet(entityP, filterP->geoproperty);
  if (geojsonP == NULL)
    return false;  // entity has no matching geoproperty

  LdGeoRelType rel = filterP->geoRel->rel;

  //
  // "near" — haversine distance on Points
  //
  if (rel == LdGeoNear)
  {
    // Entity point
    double entityLon = 0, entityLat = 0;
    KjNode* typeP = kjLookup(geojsonP, "type");
    KjNode* coordsP = kjLookup(geojsonP, "coordinates");

    if (typeP == NULL || strcmp(typeP->value.s, "Point") != 0 || coordsP == NULL)
      return false;

    KjNode* lonNode = coordsP->value.firstChildP;
    KjNode* latNode = (lonNode != NULL) ? lonNode->next : NULL;
    if (lonNode == NULL || latNode == NULL)
      return false;

    entityLon = (lonNode->type == KjFloat) ? lonNode->value.f : (double) lonNode->value.i;
    entityLat = (latNode->type == KjFloat) ? latNode->value.f : (double) latNode->value.i;

    // Reference point from filter coordinates (JSON string like "[-3.703,40.417]")
    double refLon = 0, refLat = 0;

    // Parse coordinates string — simple extraction for Point: [lon, lat]
    const char* s = filterP->coordinates;
    if (s == NULL) return false;

    // Skip '['
    while (*s && *s != '[') s++;
    if (*s == '[') s++;
    refLon = strtod(s, (char**) &s);
    while (*s == ',' || *s == ' ') s++;
    refLat = strtod(s, NULL);

    double distance = haversineDistance(entityLon, entityLat, refLon, refLat);

    if (filterP->geoRel->maxDistance >= 0 && distance > filterP->geoRel->maxDistance)
      return false;
    if (filterP->geoRel->minDistance >= 0 && distance < filterP->geoRel->minDistance)
      return false;

    if (distanceP != NULL)
      *distanceP = distance;

    return true;
  }

  //
  // Topological predicates — use GEOS
  //
  GEOSGeometry* refGeom = geojsonToGeos(filterP->geometry, filterP->coordinates);
  if (refGeom == NULL)
    return false;

  GEOSGeometry* entityGeom = entityGeoToGeos(geojsonP);
  if (entityGeom == NULL)
  {
    GEOSGeom_destroy_r(geosCtx, refGeom);
    return false;
  }

  bool match = geoRelEval(rel, refGeom, entityGeom);

  GEOSGeom_destroy_r(geosCtx, entityGeom);
  GEOSGeom_destroy_r(geosCtx, refGeom);

  return match;
}



// -----------------------------------------------------------------------------
//
// csrGeoMatchOverlap - see header
//
// Conservative "possibly contains" filter for CSR Discovery and DistOp
// dispatch. Compares the geoQ reference geometry against a CSR's stored
// geo-coverage geometry. If the CSR has no geometry for the queried
// property (csrGeoP NULL), the CSR is unconstrained: the function returns
// true so the dispatcher keeps it as a candidate.
//
bool csrGeoMatchOverlap(KjNode* csrGeoP, LdGeoRel* geoRel, const char* geometry, const char* coordinates)
{
  if (geoRel == NULL || geometry == NULL || coordinates == NULL)
    return true;  // no geo constraint
  //
  // A CSR without the named geo field MATCHES, and that is the semantics rather
  // than a fallback.
  //
  // A registration's location / observationSpace / operationSpace are
  // RESTRICTIONS on what the source covers - § 5.2.9 defines each as the
  // geographic area that "includes the ... spaces of all entities ... for which
  // the Context Source may be able to provide information". No restriction means
  // no limit: such a registration covers this area, and every other area on
  // earth and beyond.
  //
  // Compare geoMatch() above, where a missing GeoProperty means the opposite and
  // returns false. That is § 7.2.4 - "Entities which do not convey the target
  // GeoProperty of the query shall be considered as non-matching" - and it is a
  // different kind of absence: an Entity's GeoProperty is a FACT about where the
  // Entity is, so not having one cannot match a geometry. A CSR's is a CONSTRAINT
  // on what it serves, so not having one constrains nothing.
  //
  // Same NULL, opposite meaning. Worth stating, because "be permissive" would be
  // the wrong reason to arrive at the right answer, and would not survive the
  // next person tidying it up.
  //
  if (csrGeoP == NULL)
    return true;

  GEOSGeometry* refGeom = geojsonToGeos(geometry, coordinates);
  if (refGeom == NULL)
    return true;  // can't parse query geometry — be permissive

  GEOSGeometry* csrGeom = entityGeoToGeos(csrGeoP);
  if (csrGeom == NULL)
  {
    GEOSGeom_destroy_r(geosCtx, refGeom);
    return true;  // CSR geometry malformed — pass through, downstream filter will catch
  }

  bool match = false;

  if (geoRel->rel == LdGeoNear)
  {
    if (geoRel->maxDistance < 0)
    {
      // No maxDistance bound — every CSR is a candidate.
      match = true;
    }
    else
    {
      double distanceMeters = csrDistanceMeters(refGeom, csrGeom);
      if (distanceMeters >= 0)
        match = (distanceMeters <= geoRel->maxDistance);
    }
  }
  else if (geoRel->rel == LdGeoDisjoint)
  {
    //
    // ⚠️ disjoint CANNOT be pruned, and collapsing it to "intersects" got it
    // exactly backwards.
    //
    // The soundness argument for every other relation runs: the CSR field
    // INCLUDES all its entities' geometries (§ 5.2.9), so entity ⊆ E, and a
    // query asking for entities that touch R needs E to touch R. No overlap,
    // no possible match, prune.
    //
    // For disjoint the argument inverts. An entity disjoint from R may sit
    // anywhere outside R, so E touching R rules nothing out - and if E does
    // NOT touch R, then every entity behind this CSR is disjoint from R and
    // they ALL match. The old code pruned precisely that case, dropping the
    // sources whose entities were guaranteed answers.
    //
    // The only prunable case would be E entirely inside R, and even that
    // depends on boundary handling for a gain nobody will notice. So: never
    // prune on disjoint.
    //
    match = true;
  }
  else
  {
    //
    // Everything else collapses to "intersects", which is the conservative
    // direction: within / contains / equals / overlaps / intersects all
    // require the entity's geometry to touch R, and the entity is inside E, so
    // E must touch R too. A CSR that passes may still hold nothing - the
    // envelope is a superset, not a union - so a pass here is a candidate to
    // ASK, never an answer.
    //
    match = (GEOSIntersects_r(geosCtx, refGeom, csrGeom) == 1);
  }

  GEOSGeom_destroy_r(geosCtx, csrGeom);
  GEOSGeom_destroy_r(geosCtx, refGeom);

  return match;
}


// -----------------------------------------------------------------------------
//
// csrGeoMatchExact - see header
//
bool csrGeoMatchExact(KjNode* csrGeoP, LdGeoRel* geoRel, const char* geometry, const char* coordinates)
{
  if (geoRel == NULL || geometry == NULL || coordinates == NULL)
    return true;                      // no geo constraint

  //
  // No geometry for the queried property. Same reasoning as the dispatch
  // filter: these fields are restrictions, and an absent one restricts
  // nothing - so the registration is returned.
  //
  // ⚠️ Except for disjoint, where "unrestricted" is what makes it match rather
  // than an exemption from matching: a registration that covers everywhere
  // covers the area outside R as well.
  //
  if (csrGeoP == NULL)
    return true;

  GEOSGeometry* refGeom = geojsonToGeos(geometry, coordinates);
  if (refGeom == NULL)
    return true;                      // unparseable query geometry - be permissive

  GEOSGeometry* csrGeom = entityGeoToGeos(csrGeoP);
  if (csrGeom == NULL)
  {
    //
    // A CSR field that is a bare GeoJSON geometry rather than a GeoProperty
    // object lands here, as does a malformed one. entityGeoToGeos expects the
    // GeoProperty's "value" shape; the CSR stores the geometry directly.
    //
    GEOSGeom_destroy_r(geosCtx, refGeom);
    return true;
  }

  bool match;

  if (geoRel->rel == LdGeoNear)
  {
    if (geoRel->maxDistance < 0)
      match = true;
    else
    {
      double d = csrDistanceMeters(refGeom, csrGeom);
      match = (d >= 0) && (d <= geoRel->maxDistance);
    }
  }
  else
    match = geoRelEval(geoRel->rel, refGeom, csrGeom);

  GEOSGeom_destroy_r(geosCtx, csrGeom);
  GEOSGeom_destroy_r(geosCtx, refGeom);

  return match;
}

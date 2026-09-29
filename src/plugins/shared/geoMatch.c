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

#include <pthread.h>                                     // pthread_mutex_t
#include <geos_c.h>                                      // GEOS C API

#include "corTree/CorNode.h"                             // CorNode
#include "corTree/corTreeLookup.h"                       // corTreeLookup

#include "corNgsild/LdGeoRel.h"                           // LdGeoRel, LdGeoRelType
#include "corNgsild/LdVocab.h"                            // LD_VOCAB_*
#include "db/DbQueryFilter.h"                            // DbQueryFilter

#include "shared/geoMatch.h"                             // Own interface



// -----------------------------------------------------------------------------
//
// GeosCtx - a pool of GEOS context handles
//
// GEOS is thread-safe PER CONTEXT: its _r functions may run concurrently only on different
// context handles. There was ONE for the whole process, used by every geo query at once (corDB
// queries run under a READ lock, so concurrently) and by every subscription match. So each
// public function below takes a context from this pool for the length of the call and gives it
// back: the pool grows to the most geo operations ever in flight at once, and never shrinks.
// Taking one is a mutex held for a pointer swap - nothing next to what a call does with it.
//
//
// Each context also keeps the last REFERENCE geometry it was asked about - the query's, which
// is the same for every entity (and every registration) a query tests. Parsed once and GEOS-
// prepared, and found again by a strcmp of its coordinates: a query over n entities parsed it
// n times, building a GeoJSON string and a reader for each. The cache is as private to its
// caller as the context it lives in.
//
typedef struct GeosCtx
{
  GEOSContextHandle_t            handle;
  char*                          refGeometry;      // the cached reference: its geometry type ...
  char*                          refCoordinates;   // ... and coordinates, as given
  GEOSGeometry*                  refGeom;          // parsed
  const GEOSPreparedGeometry*    refPrep;          // prepared (NULL if GEOS would not)
  struct GeosCtx*                nextFree;         // free list
  struct GeosCtx*                nextAll;          // every context ever made - for geoMatchClose
} GeosCtx;

static pthread_mutex_t  geosPoolMutex = PTHREAD_MUTEX_INITIALIZER;
static GeosCtx*         geosFree      = NULL;
static GeosCtx*         geosAll       = NULL;



// -----------------------------------------------------------------------------
//
// geosCtxTake - a context no other thread is using; NULL if none can be made
//
static GeosCtx* geosCtxTake(void)
{
  pthread_mutex_lock(&geosPoolMutex);

  GeosCtx* ctxP = geosFree;

  if (ctxP != NULL)
    geosFree = ctxP->nextFree;

  pthread_mutex_unlock(&geosPoolMutex);

  if (ctxP != NULL)
    return ctxP;

  ctxP = (GeosCtx*) calloc(1, sizeof(GeosCtx));
  if (ctxP == NULL)
    return NULL;

  ctxP->handle = GEOS_init_r();
  if (ctxP->handle == NULL)
  {
    free(ctxP);
    return NULL;
  }

  pthread_mutex_lock(&geosPoolMutex);
  ctxP->nextAll = geosAll;
  geosAll       = ctxP;
  pthread_mutex_unlock(&geosPoolMutex);

  return ctxP;
}



// -----------------------------------------------------------------------------
//
// geosCtxGive - back to the pool
//
static void geosCtxGive(GeosCtx* ctxP)
{
  pthread_mutex_lock(&geosPoolMutex);
  ctxP->nextFree = geosFree;
  geosFree       = ctxP;
  pthread_mutex_unlock(&geosPoolMutex);
}



// -----------------------------------------------------------------------------
//
// geoMatchInit / geoMatchClose
//
void geoMatchInit(void)
{
  // Nothing to do - contexts are made on first use, by geosCtxTake
}

void geoMatchClose(void)
{
  pthread_mutex_lock(&geosPoolMutex);

  GeosCtx* ctxP = geosAll;

  geosAll  = NULL;
  geosFree = NULL;

  pthread_mutex_unlock(&geosPoolMutex);

  while (ctxP != NULL)
  {
    GeosCtx* nextP = ctxP->nextAll;

    if (ctxP->refPrep != NULL)
      GEOSPreparedGeom_destroy_r(ctxP->handle, ctxP->refPrep);
    if (ctxP->refGeom != NULL)
      GEOSGeom_destroy_r(ctxP->handle, ctxP->refGeom);
    free(ctxP->refGeometry);
    free(ctxP->refCoordinates);
    GEOS_finish_r(ctxP->handle);
    free(ctxP);
    ctxP = nextP;
  }
}



// -----------------------------------------------------------------------------
//
// geojsonToGeos - build a GEOS geometry from geometry type + coordinates string
//
// geometry:    "Point", "Polygon", "LineString", "MultiPolygon", etc.
// coordinates: JSON array string, e.g. "[-3.703,40.417]" or "[[[...]]]"
//
static GEOSGeometry* geojsonToGeos(GEOSContextHandle_t geosCtx, const char* geometry, const char* coordinates)
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
// against it - fall back to corTreeLookup(entityP, "https://uri.etsi.org/ngsi-ld/location")
// - was inert, looked like the test failing to discriminate, and sent the
// investigation at the test instead of at the comment.
//
// Returns the "value" node of the GeoProperty (which is a GeoJSON object).
//
static CorNode* entityGeoPropGet(CorNode* entityP, const char* geoproperty)
{
  CorNode* attrP = corTreeLookup(entityP, geoproperty);
  if (attrP == NULL || attrP->type != CorObject)
    return NULL;

  // First child is the default instance ("@none")
  CorNode* instP = attrP->value.head;
  if (instP == NULL || instP->type != CorObject)
    return NULL;

  // Get "value" from the instance — should be a GeoJSON object
  return corTreeLookup(instP, "value");
}



// -----------------------------------------------------------------------------
//
// coordsRender - render a CorNode coordinate array to a JSON string
//
// Returns number of characters written, or -1 on overflow.
//
static int coordsRender(CorNode* nodeP, char* buf, int bufSize)
{
  int pos = 0;

  if (nodeP->type == CorArray)
  {
    if (pos < bufSize) buf[pos++] = '[';
    bool first = true;
    for (CorNode* childP = nodeP->value.head; childP != NULL; childP = childP->next)
    {
      if (!first && pos < bufSize) buf[pos++] = ',';
      first = false;
      int written = coordsRender(childP, buf + pos, bufSize - pos);
      if (written < 0) return -1;
      pos += written;
    }
    if (pos < bufSize) buf[pos++] = ']';
  }
  else if (nodeP->type == CorFloat)
  {
    pos += snprintf(buf + pos, bufSize - pos, "%.15g", nodeP->value.f);
  }
  else if (nodeP->type == CorInt)
  {
    pos += snprintf(buf + pos, bufSize - pos, "%lld", (long long) nodeP->value.i);
  }

  return pos;
}



// -----------------------------------------------------------------------------
//
// refGet - the reference geometry (geometry + coordinates), parsed and prepared, from the cache
//
// Owned by the context: never destroyed by the caller. NULL if it does not parse (not cached,
// so the caller's own error path runs every time, as it did).
//
static GEOSGeometry* refGet(GeosCtx* ctxP, const char* geometry, const char* coordinates, const GEOSPreparedGeometry** prepPP)
{
  *prepPP = NULL;

  if ((geometry == NULL) || (coordinates == NULL))   // nothing to key on: parsed as it always was, not cached
    return geojsonToGeos(ctxP->handle, geometry, coordinates);

  if ((ctxP->refGeom != NULL) &&
      (strcmp(ctxP->refCoordinates, coordinates) == 0) &&
      (strcmp(ctxP->refGeometry, geometry) == 0))
  {
    *prepPP = ctxP->refPrep;
    return ctxP->refGeom;
  }

  GEOSGeometry* geomP = geojsonToGeos(ctxP->handle, geometry, coordinates);

  *prepPP = NULL;
  if (geomP == NULL)
    return NULL;

  char* geometryCopy    = strdup(geometry);
  char* coordinatesCopy = strdup(coordinates);

  if ((geometryCopy == NULL) || (coordinatesCopy == NULL))
  {
    //
    // Not cached - the caller still gets its geometry, and gives it back via refDone
    //
    free(geometryCopy);
    free(coordinatesCopy);
    return geomP;
  }

  if (ctxP->refPrep != NULL)
    GEOSPreparedGeom_destroy_r(ctxP->handle, ctxP->refPrep);
  if (ctxP->refGeom != NULL)
    GEOSGeom_destroy_r(ctxP->handle, ctxP->refGeom);
  free(ctxP->refGeometry);
  free(ctxP->refCoordinates);

  ctxP->refGeom        = geomP;
  ctxP->refPrep        = GEOSPrepare_r(ctxP->handle, geomP);
  ctxP->refGeometry    = geometryCopy;
  ctxP->refCoordinates = coordinatesCopy;

  *prepPP = ctxP->refPrep;
  return geomP;
}



// -----------------------------------------------------------------------------
//
// refDone - the reference is finished with: destroyed only if refGet could not cache it
//
static void refDone(GeosCtx* ctxP, GEOSGeometry* refGeom)
{
  if ((refGeom != NULL) && (refGeom != ctxP->refGeom))
    GEOSGeom_destroy_r(ctxP->handle, refGeom);
}



// -----------------------------------------------------------------------------
//
// entityGeoToGeos - convert an entity's GeoJSON value node to a GEOS geometry
//
static GEOSGeometry* entityGeoToGeos(GEOSContextHandle_t geosCtx, CorNode* geojsonP)
{
  CorNode* typeP  = corTreeLookup(geojsonP, "type");
  CorNode* coordsP = corTreeLookup(geojsonP, "coordinates");

  if (typeP == NULL || typeP->type != CorString || coordsP == NULL)
    return NULL;

  char coordBuf[4096];
  int  pos = coordsRender(coordsP, coordBuf, sizeof(coordBuf));
  if (pos <= 0 || pos >= (int) sizeof(coordBuf))
    return NULL;
  coordBuf[pos] = 0;

  return geojsonToGeos(geosCtx, typeP->value.s, coordBuf);
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
static bool geoEntityValidateWith(GeosCtx* ctxP, CorNode* entityP)
{
  GEOSContextHandle_t geosCtx = ctxP->handle;

  if (entityP == NULL || entityP->type != CorObject)
    return true;

  for (CorNode* attrP = entityP->value.head; attrP != NULL; attrP = attrP->next)
  {
    if (attrP->type != CorObject)
      continue;

    for (CorNode* instP = attrP->value.head; instP != NULL; instP = instP->next)
    {
      if (instP->type != CorObject)
        continue;

      CorNode* typeP = corTreeLookup(instP, "type");
      if (typeP == NULL || typeP->type != CorString || strcmp(typeP->value.s, "GeoProperty") != 0)
        continue;

      CorNode* geojsonP = corTreeLookup(instP, "value");
      if (geojsonP == NULL || geojsonP->type != CorObject)
        continue;

      GEOSGeometry* geom = entityGeoToGeos(geosCtx, geojsonP);
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
static double csrDistanceMeters(GEOSContextHandle_t geosCtx, const GEOSGeometry* refGeom, const GEOSGeometry* csrGeom)
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
static bool geoRelEval(GEOSContextHandle_t geosCtx, LdGeoRelType rel, const GEOSGeometry* refGeom, const GEOSPreparedGeometry* refPrep, const GEOSGeometry* targetGeom)
{
  //
  // The reference prepared: GEOS indexes it once, and each predicate against it is then far
  // cheaper. Same answers - a prepared predicate is exact. The direction is kept: "within"
  // asks whether the REFERENCE contains the target, "contains" whether it lies within it.
  // equals has no prepared form.
  //
  if (refPrep != NULL)
  {
    switch (rel)
    {
    case LdGeoWithin:      return (GEOSPreparedContains_r(geosCtx, refPrep, targetGeom) == 1);
    case LdGeoContains:    return (GEOSPreparedWithin_r(geosCtx, refPrep, targetGeom) == 1);
    case LdGeoIntersects:  return (GEOSPreparedIntersects_r(geosCtx, refPrep, targetGeom) == 1);
    case LdGeoDisjoint:    return (GEOSPreparedDisjoint_r(geosCtx, refPrep, targetGeom) == 1);
    case LdGeoOverlaps:    return (GEOSPreparedOverlaps_r(geosCtx, refPrep, targetGeom) == 1);
    default:               break;
    }
  }

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



static bool geoMatchWith(GeosCtx* ctxP, CorNode* entityP, DbQueryFilter* filterP, double* distanceP)
{
  GEOSContextHandle_t geosCtx = ctxP->handle;

  if (distanceP != NULL)
    *distanceP = -1;

  if (filterP == NULL || filterP->geoRel == NULL)
    return true;  // no geo filter

  // Find the entity's geoproperty
  CorNode* geojsonP = entityGeoPropGet(entityP, filterP->geoproperty);
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
    CorNode* typeP = corTreeLookup(geojsonP, "type");
    CorNode* coordsP = corTreeLookup(geojsonP, "coordinates");

    if (typeP == NULL || strcmp(typeP->value.s, "Point") != 0 || coordsP == NULL)
      return false;

    CorNode* lonNode = coordsP->value.head;
    CorNode* latNode = (lonNode != NULL) ? lonNode->next : NULL;
    if (lonNode == NULL || latNode == NULL)
      return false;

    entityLon = (lonNode->type == CorFloat) ? lonNode->value.f : (double) lonNode->value.i;
    entityLat = (latNode->type == CorFloat) ? latNode->value.f : (double) latNode->value.i;

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
  const GEOSPreparedGeometry* refPrep = NULL;
  GEOSGeometry*               refGeom = refGet(ctxP, filterP->geometry, filterP->coordinates, &refPrep);
  if (refGeom == NULL)
    return false;

  GEOSGeometry* entityGeom = entityGeoToGeos(geosCtx, geojsonP);
  if (entityGeom == NULL)
  {
    refDone(ctxP, refGeom);
    return false;
  }

  bool match = geoRelEval(geosCtx, rel, refGeom, refPrep, entityGeom);

  GEOSGeom_destroy_r(geosCtx, entityGeom);
  refDone(ctxP, refGeom);

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
static bool csrGeoMatchOverlapWith(GeosCtx* ctxP, CorNode* csrGeoP, LdGeoRel* geoRel, const char* geometry, const char* coordinates)
{
  GEOSContextHandle_t geosCtx = ctxP->handle;

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

  const GEOSPreparedGeometry* refPrep = NULL;
  GEOSGeometry*               refGeom = refGet(ctxP, geometry, coordinates, &refPrep);
  if (refGeom == NULL)
    return true;  // can't parse query geometry — be permissive

  GEOSGeometry* csrGeom = entityGeoToGeos(geosCtx, csrGeoP);
  if (csrGeom == NULL)
  {
    refDone(ctxP, refGeom);
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
      double distanceMeters = csrDistanceMeters(geosCtx, refGeom, csrGeom);
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
    match = (refPrep != NULL) ? (GEOSPreparedIntersects_r(geosCtx, refPrep, csrGeom) == 1) : (GEOSIntersects_r(geosCtx, refGeom, csrGeom) == 1);
  }

  GEOSGeom_destroy_r(geosCtx, csrGeom);
  refDone(ctxP, refGeom);

  return match;
}


// -----------------------------------------------------------------------------
//
// csrGeoMatchExact - see header
//
static bool csrGeoMatchExactWith(GeosCtx* ctxP, CorNode* csrGeoP, LdGeoRel* geoRel, const char* geometry, const char* coordinates)
{
  GEOSContextHandle_t geosCtx = ctxP->handle;

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

  const GEOSPreparedGeometry* refPrep = NULL;
  GEOSGeometry*               refGeom = refGet(ctxP, geometry, coordinates, &refPrep);
  if (refGeom == NULL)
    return true;                      // unparseable query geometry - be permissive

  GEOSGeometry* csrGeom = entityGeoToGeos(geosCtx, csrGeoP);
  if (csrGeom == NULL)
  {
    //
    // A CSR field that is a bare GeoJSON geometry rather than a GeoProperty
    // object lands here, as does a malformed one. entityGeoToGeos expects the
    // GeoProperty's "value" shape; the CSR stores the geometry directly.
    //
    refDone(ctxP, refGeom);
    return true;
  }

  bool match;

  if (geoRel->rel == LdGeoNear)
  {
    if (geoRel->maxDistance < 0)
      match = true;
    else
    {
      double d = csrDistanceMeters(geosCtx, refGeom, csrGeom);
      match = (d >= 0) && (d <= geoRel->maxDistance);
    }
  }
  else
    match = geoRelEval(geosCtx, geoRel->rel, refGeom, refPrep, csrGeom);

  GEOSGeom_destroy_r(geosCtx, csrGeom);
  refDone(ctxP, refGeom);

  return match;
}



// -----------------------------------------------------------------------------
//
// geoEntityValidate - on a context of its own for the length of the call
//
bool geoEntityValidate(CorNode* entityP)
{
  GeosCtx* ctxP = geosCtxTake();

  if (ctxP == NULL)
    return true;

  bool r = geoEntityValidateWith(ctxP, entityP);

  geosCtxGive(ctxP);
  return r;
}



// -----------------------------------------------------------------------------
//
// geoMatch - on a context of its own for the length of the call
//
bool geoMatch(CorNode* entityP, DbQueryFilter* filterP, double* distanceP)
{
  GeosCtx* ctxP = geosCtxTake();

  if (ctxP == NULL)
    return false;

  bool r = geoMatchWith(ctxP, entityP, filterP, distanceP);

  geosCtxGive(ctxP);
  return r;
}



// -----------------------------------------------------------------------------
//
// csrGeoMatchOverlap - on a context of its own for the length of the call
//
bool csrGeoMatchOverlap(CorNode* csrGeoP, LdGeoRel* geoRel, const char* geometry, const char* coordinates)
{
  GeosCtx* ctxP = geosCtxTake();

  if (ctxP == NULL)
    return false;

  bool r = csrGeoMatchOverlapWith(ctxP, csrGeoP, geoRel, geometry, coordinates);

  geosCtxGive(ctxP);
  return r;
}



// -----------------------------------------------------------------------------
//
// csrGeoMatchExact - on a context of its own for the length of the call
//
bool csrGeoMatchExact(CorNode* csrGeoP, LdGeoRel* geoRel, const char* geometry, const char* coordinates)
{
  GeosCtx* ctxP = geosCtxTake();

  if (ctxP == NULL)
    return false;

  bool r = csrGeoMatchExactWith(ctxP, csrGeoP, geoRel, geometry, coordinates);

  geosCtxGive(ctxP);
  return r;
}

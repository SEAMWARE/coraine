//
// FILE            postEntityMap.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/entityMaps — § 5.14.4 / § 6.34.3.2. Accepts a Query
// object (§ 5.2.23) in the body; translates each field to the same
// internal state used by GET /entityMaps' URL parameters (by handing
// each translated name/value pair to ldParamHook), then delegates to
// createEntityMap which runs the distributed query and returns the
// resulting EntityMap with 201 Created.
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp, strlen, strcpy, memcpy
#include <stdio.h>                                   // snprintf

#include "swRest/SwRestState.h"                      // swRest

#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjRender.h"                          // kjFastRender
#include "kjson/kjRenderSize.h"                      // kjFastRenderSize

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldParamHook

#include "serviceRoutines/createEntityMap.h"         // createEntityMap
#include "serviceRoutines/postEntityMap.h"           // Own interface



// -----------------------------------------------------------------------------
//
// arrayJoin - comma-join a KjArray of strings into a single allocation.
//
// Used to map Query body array fields (attrs[], pick[], omit[],
// datasetId[]) into the comma-separated syntax ldParamHook expects.
//
static const char* arrayJoin(KjNode* arrP)
{
  if (arrP == NULL || arrP->type != KjArray)
    return NULL;

  int total = 0;
  int n     = 0;
  for (KjNode* c = arrP->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->type != KjString) continue;
    total += strlen(c->value.s) + 1;
    n++;
  }
  if (n == 0)
    return NULL;

  char* buf = (char*) kaAlloc(&swRest.kalloc, total + 1);
  int pos = 0;
  for (KjNode* c = arrP->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->type != KjString) continue;
    if (pos > 0) buf[pos++] = ',';
    int len = strlen(c->value.s);
    memcpy(buf + pos, c->value.s, len);
    pos += len;
  }
  buf[pos] = 0;
  return buf;
}



// -----------------------------------------------------------------------------
//
// collectFromEntities - pull id / idPattern / type out of a Query's
// `entities` EntitySelector[] (§ 5.2.33) and feed them to ldParamHook
// in the URL-param form it understands.
//
// Multiple selectors are merged: all ids joined, all types joined. Each
// unique idPattern fed through separately would need param-accumulation;
// instead the first idPattern seen wins (the URL form only supports one).
//
static void collectFromEntities(KjNode* entsArr)
{
  if (entsArr == NULL || entsArr->type != KjArray)
    return;

  int idLen = 0, typeLen = 0, idCount = 0, typeCount = 0;
  const char* firstIdPattern = NULL;

  for (KjNode* selP = entsArr->value.firstChildP; selP != NULL; selP = selP->next)
  {
    if (selP->type != KjObject) continue;

    KjNode* idP        = kjLookup(selP, "id");
    KjNode* typeP      = kjLookup(selP, "type");
    KjNode* patternP   = kjLookup(selP, "idPattern");

    if (idP != NULL && idP->type == KjString)
    {
      idLen += strlen(idP->value.s) + 1;
      idCount++;
    }
    if (typeP != NULL && typeP->type == KjString)
    {
      typeLen += strlen(typeP->value.s) + 1;
      typeCount++;
    }
    if (firstIdPattern == NULL && patternP != NULL && patternP->type == KjString)
      firstIdPattern = patternP->value.s;
  }

  if (idCount > 0)
  {
    char* buf = (char*) kaAlloc(&swRest.kalloc, idLen + 1);
    int pos = 0;
    for (KjNode* selP = entsArr->value.firstChildP; selP != NULL; selP = selP->next)
    {
      if (selP->type != KjObject) continue;
      KjNode* idP = kjLookup(selP, "id");
      if (idP == NULL || idP->type != KjString) continue;
      if (pos > 0) buf[pos++] = ',';
      int len = strlen(idP->value.s);
      memcpy(buf + pos, idP->value.s, len);
      pos += len;
    }
    buf[pos] = 0;
    ldParamHook("id", buf);
  }

  if (typeCount > 0)
  {
    char* buf = (char*) kaAlloc(&swRest.kalloc, typeLen + 1);
    int pos = 0;
    for (KjNode* selP = entsArr->value.firstChildP; selP != NULL; selP = selP->next)
    {
      if (selP->type != KjObject) continue;
      KjNode* typeP = kjLookup(selP, "type");
      if (typeP == NULL || typeP->type != KjString) continue;
      if (pos > 0) buf[pos++] = ',';
      int len = strlen(typeP->value.s);
      memcpy(buf + pos, typeP->value.s, len);
      pos += len;
    }
    buf[pos] = 0;
    ldParamHook("type", buf);
  }

  if (firstIdPattern != NULL)
    ldParamHook("idPattern", firstIdPattern);
}



// -----------------------------------------------------------------------------
//
// collectFromGeoQ - explode a GeoQuery object (§ 5.2.13) into its
// constituent URL params: georel, geometry, coordinates, geoproperty.
//
// coordinates is rendered as JSON (matches the URL form `coordinates=[...]`).
//
static void collectFromGeoQ(KjNode* geoQ)
{
  if (geoQ == NULL || geoQ->type != KjObject)
    return;

  KjNode* georel      = kjLookup(geoQ, "georel");
  KjNode* geometry    = kjLookup(geoQ, "geometry");
  KjNode* coords      = kjLookup(geoQ, "coordinates");
  KjNode* geoproperty = kjLookup(geoQ, "geoproperty");

  if (georel      != NULL && georel->type      == KjString) ldParamHook("georel",      georel->value.s);
  if (geometry    != NULL && geometry->type    == KjString) ldParamHook("geometry",    geometry->value.s);
  if (geoproperty != NULL && geoproperty->type == KjString) ldParamHook("geoproperty", geoproperty->value.s);

  if (coords != NULL)
  {
    int   bufSize = kjFastRenderSize(coords) + 1;
    char* buf     = (char*) kaAlloc(&swRest.kalloc, bufSize);
    kjFastRender(coords, buf);
    ldParamHook("coordinates", buf);
  }
}



// -----------------------------------------------------------------------------
//
// postEntityMap -
//
bool postEntityMap(void)
{
  if (swNgsild.contextError)
    return true;

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

  if (bodyP->type != KjObject)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Query body must be a JSON object");
    return true;
  }

  //
  // § 5.2.23: type is mandatory and must be "Query".
  //
  KjNode* typeP = kjLookup(bodyP, "type");
  if (typeP == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Query body must carry \"type\": \"Query\"");
    return true;
  }
  //
  // The parseHook ran JSON-LD expansion on the body, so "type": "Query"
  // is now either still the literal "Query" (if the client supplied their
  // own context that defines it as-is) or the expanded default-vocab IRI.
  // Accept both.
  //
  const char* expandedQuery = "https://uri.etsi.org/ngsi-ld/default-context/Query";
  if (typeP->type != KjString ||
      (strcmp(typeP->value.s, "Query") != 0 &&
       strcmp(typeP->value.s, expandedQuery) != 0))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Query body must carry \"type\": \"Query\"");
    return true;
  }

  //
  // Field-by-field translation. Scalars → ldParamHook directly. Arrays
  // of strings → comma-join → ldParamHook. Objects/nested structures
  // (geoQ, entities) get dedicated helpers.
  //
  for (KjNode* fP = bodyP->value.firstChildP; fP != NULL; fP = fP->next)
  {
    if (fP->name == NULL) continue;
    if (fP->name[0] == '@') continue;       // @context, @type — JSON-LD
    if (strcmp(fP->name, "type") == 0) continue;  // already validated

    if (strcmp(fP->name, "entities") == 0)
    {
      collectFromEntities(fP);
      continue;
    }

    if (strcmp(fP->name, "geoQ") == 0)
    {
      collectFromGeoQ(fP);
      continue;
    }

    if (strcmp(fP->name, "attrs")     == 0 ||
        strcmp(fP->name, "pick")      == 0 ||
        strcmp(fP->name, "omit")      == 0 ||
        strcmp(fP->name, "datasetId") == 0)
    {
      const char* joined = arrayJoin(fP);
      if (joined != NULL)
        ldParamHook(fP->name, joined);
      continue;
    }

    // Booleans → "true"/"false"
    if (fP->type == KjBoolean)
    {
      ldParamHook(fP->name, fP->value.b ? "true" : "false");
      continue;
    }

    // Plain strings pass through
    if (fP->type == KjString)
    {
      ldParamHook(fP->name, fP->value.s);
      continue;
    }

    // Numbers → string form
    if (fP->type == KjInt)
    {
      char buf[32];
      snprintf(buf, sizeof(buf), "%lld", fP->value.i);
      ldParamHook(fP->name, buf);
      continue;
    }

    // Anything else (temporalQ, aggrParams, ordering, ...) — not yet
    // wired through URL-param-style handling; silently skipped. A
    // future pass can extend this translator.
  }

  return createEntityMap();
}

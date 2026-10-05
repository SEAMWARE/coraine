//
// FILE            seRegistrationMatch.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                   // strcmp
#include <regex.h>                                    // regcomp, regexec, regfree

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corRest/CorRestState.h"                     // corRest
#include "corJson/corJsonRender.h"                    // corJsonFastRender
#include "corJson/corJsonRenderSize.h"                // corJsonFastRenderSize
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corJsonld/corLdExpand.h"                    // corLdExpand
#include "corNgsild/CorNgsild.h"                      // corNgsild
#include "corNgsild/LdGeoRel.h"                       // ldGeoRelParse
#include "corNgsild/ldQParse.h"                       // ldQParseStored
#include "corNgsild/ldEntityMatch.h"                  // ldEntityMatchQ

#include "db/DbDriver.h"                              // db

#include "serviceExecution/seRegistrationMatch.h"     // Own interface



// -----------------------------------------------------------------------------
//
// typeIn - is 'type' one of typeV?
//
static bool typeIn(const char* type, char** typeV)
{
  for (int ix = 0; typeV[ix] != NULL; ix++)
  {
    if (strcmp(typeV[ix], type) == 0)
      return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// selectorMatches - one member of 'entities': { type, id | idPattern }
//
static bool selectorMatches(CorNode* selP, const char* entityId, char** typeV)
{
  CorNode* typeP      = corTreeLookup(selP, "type");
  CorNode* idP        = corTreeLookup(selP, "id");
  CorNode* idPatternP = corTreeLookup(selP, "idPattern");

  if ((typeV != NULL) && ((typeP == NULL) || (typeP->type != CorString) || (typeIn(typeP->value.s, typeV) == false)))
    return false;

  if (entityId == NULL)
    return true;

  if ((idP != NULL) && (idP->type == CorString))
    return strcmp(idP->value.s, entityId) == 0;

  if ((idPatternP != NULL) && (idPatternP->type == CorString))
  {
    regex_t re;

    if (regcomp(&re, idPatternP->value.s, REG_EXTENDED | REG_NOSUB) != 0)
      return false;

    bool match = (regexec(&re, entityId, 0, NULL, 0) == 0);

    regfree(&re);
    return match;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// geoQMatches - the registration's geoQ against the entity (the subscription's way: db.geoMatchFunc)
//
static bool geoQMatches(CorNode* geoQP, CorNode* entityP)
{
  if (db.geoMatchFunc == NULL)
    return false;                                     // no geo in this build/plugin: it cannot be said to match

  CorNode* georelP   = corTreeLookup(geoQP, "georel");
  CorNode* geometryP = corTreeLookup(geoQP, "geometry");
  CorNode* coordsP   = corTreeLookup(geoQP, "coordinates");
  CorNode* geopropP  = corTreeLookup(geoQP, "geoproperty");
  char*    coords    = NULL;

  if ((georelP == NULL) || (georelP->type != CorString) || (geometryP == NULL) || (geometryP->type != CorString) || (coordsP == NULL))
    return false;

  if (coordsP->type == CorString)
    coords = coordsP->value.s;
  else
  {
    int size = corJsonFastRenderSize(coordsP) + 1;

    coords = (char*) corAlloc(&corRest.kalloc, size);
    corJsonFastRender(coordsP, coords);
  }

  const char* geoproperty = ((geopropP != NULL) && (geopropP->type == CorString)) ? geopropP->value.s : "location";

  geoproperty = corLdExpand(corNgsild.contextP, geoproperty, &corRest.kalloc, NULL, NULL);

  LdGeoRel* geoRelP = ldGeoRelParse(georelP->value.s, &corRest.kalloc);

  if (geoRelP == NULL)
    return false;

  return db.geoMatchFunc(entityP, geoRelP, geometryP->value.s, coords, geoproperty);
}



// -----------------------------------------------------------------------------
//
// seRegistrationMatches -
//
bool seRegistrationMatches(CorNode* regP, const char* entityId, char** typeV, CorNode* entityP)
{
  CorNode* entitiesP = corTreeLookup(regP, "entities");
  bool     selected  = false;

  for (CorNode* selP = (entitiesP != NULL) ? entitiesP->value.head : NULL; (selP != NULL) && (selected == false); selP = selP->next)
    selected = selectorMatches(selP, entityId, typeV);

  if ((selected == false) || (entityP == NULL))
    return selected;

  CorNode* qP = corTreeLookup(regP, "q");

  if ((qP != NULL) && (qP->type == CorString))
  {
    LdQNode* qExprP = ldQParseStored(qP->value.s, &corRest.kalloc);

    if ((qExprP == NULL) || (ldEntityMatchQ(entityP, qExprP) == false))
      return false;
  }

  CorNode* geoQP = corTreeLookup(regP, "geoQ");

  if ((geoQP != NULL) && (geoQP->type == CorObject) && (geoQMatches(geoQP, entityP) == false))
    return false;

  return true;
}

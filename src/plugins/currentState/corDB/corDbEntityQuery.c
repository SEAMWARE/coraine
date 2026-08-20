//
// FILE            corDbEntityQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <regex.h>                                     // regcomp, regexec, regfree
#include <stdlib.h>                                    // strtod, qsort
#include <string.h>                                    // strcmp

#include "kalloc/kaAlloc.h"                           // kaAlloc
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjArray, kjClone, kjFloat, kjChildAdd
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjLookup.h"                           // kjLookup
#include "corRest/CorRestState.h"                       // corRest
#include "corNgsild/LdQ.h"                              // LdQNode
#include "corNgsild/LdVocab.h"                         // LD_VOCAB_SCOPE
#include "corNgsild/LdScopeExpr.h"                     // LdScopeExpr
#include "corNgsild/LdGeoRel.h"                         // LdGeoRel, LdGeoNear
#include "corNgsild/ldEntityMatch.h"                    // ldEntityMatchType, ldEntityMatchScope, ldEntityMatchQ

#include "db/DbDriver.h"                              // DB_OK, Tenant
#include "currentState/corDB/corDbStore.h"          // corDbEntities
#include "currentState/corDB/corDbGeoMatch.h"       // corDbGeoMatch
#include "currentState/corDB/corDbEntityQuery.h"    // Own interface



// Type, scope, and q-filter matchers are shared via ldEntityMatch.h



// matchStringV - check if value is in a NULL-terminated string array
//
static bool matchStringV(const char* value, char** strV)
{
  for (int ix = 0; strV[ix] != NULL; ix++)
  {
    if (strcmp(value, strV[ix]) == 0)
      return true;
  }
  return false;
}



// -----------------------------------------------------------------------------
//
// GeoCand - a matched entity plus its geo distance (metres; -1 if not a near
// query). Collected before pagination so a near query can be sorted by distance.
//
typedef struct GeoCand
{
  KjNode* eP;
  double  dist;
} GeoCand;



// geoCandCmp - ascending by distance (nearest first), for qsort
//
static int geoCandCmp(const void* a, const void* b)
{
  double da = ((const GeoCand*) a)->dist;
  double db = ((const GeoCand*) b)->dist;

  if (da < db) return -1;
  if (da > db) return  1;
  return 0;
}



// distCandCmp - § 7.6.2.2 sort-by-distance ordering. Entities that convey the
// GeoProperty (dist >= 0) rank ahead of those that do not (dist < 0), which sort
// last; among geo-bearing entities the order is ascending, or descending when
// distDescCand is set (dist-desc). qsort has no context arg → thread-local flag.
//
static __thread bool distDescCand;

static int distCandCmp(const void* a, const void* b)
{
  double da = ((const GeoCand*) a)->dist;
  double db = ((const GeoCand*) b)->dist;
  bool   ga = (da >= 0);
  bool   gb = (db >= 0);

  if (ga != gb) return ga ? -1 : 1;   // geo-bearing first
  if (!ga)      return 0;             // both lack the GeoProperty

  int cmp = (da < db) ? -1 : (da > db) ? 1 : 0;
  return distDescCand ? -cmp : cmp;
}



// -----------------------------------------------------------------------------
//
// corDbEntityQuery -
//
int corDbEntityQuery(Tenant* tenantP, DbQueryFilter* filterP, KjNode** arrayPP)
{
  KjNode* entities = corDbEntities(tenantP);
  KjNode* arrayP   = kjArray(corRest.kjsonP, NULL);
  int     limit    = (filterP != NULL) ? filterP->limit  : 0;
  int     offset   = (filterP != NULL) ? filterP->offset : 0;

  //
  // Count the store so the candidate array can be sized up front.
  //
  int total = 0;
  for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next) total++;

  GeoCand* cands = (GeoCand*) kaAlloc(&corRest.kalloc, sizeof(GeoCand) * (total > 0 ? total : 1));
  int      nCand = 0;

  // § 7.6.2.2 sort-by-distance — a synthetic near filter reused per entity to
  // compute the distance from the orderFrom Point to the named GeoProperty.
  DbQueryFilter  distFilter;
  LdGeoRel       distRel = { LdGeoNear, -1, -1 };
  DbQueryFilter* distFilterP = NULL;
  if (filterP != NULL && filterP->distGeoproperty != NULL && filterP->geoRel == NULL)
  {
    distFilter             = *filterP;
    distFilter.geoRel      = &distRel;
    distFilter.geometry    = (char*) "Point";
    distFilter.coordinates = filterP->distFrom;
    distFilter.geoproperty = filterP->distGeoproperty;
    distFilterP            = &distFilter;
  }

  for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
  {
    KjNode* entityIdP = kjLookup(eP, "id");
    const char* entityId = (entityIdP != NULL && entityIdP->type == KjString) ? entityIdP->value.s : NULL;

    //
    // Filter by id
    //
    if (filterP != NULL && filterP->idV != NULL)
    {
      if (entityId == NULL || !matchStringV(entityId, filterP->idV))
        continue;
    }

    //
    // Filter by idPattern
    //
    if (filterP != NULL && filterP->idPattern != NULL)
    {
      regex_t re;

      if (entityId == NULL || regcomp(&re, filterP->idPattern, REG_EXTENDED | REG_NOSUB) != 0 || regexec(&re, entityId, 0, NULL, 0) != 0)
      {
        regfree(&re);
        continue;
      }

      regfree(&re);
    }

    //
    // Filter by type
    //
    if (filterP != NULL && filterP->typeExpr != NULL)
    {
      KjNode* typeP = kjLookup(eP, "type");

      if (!ldEntityMatchType(typeP, filterP->typeExpr))
        continue;
    }
    else if (filterP != NULL && filterP->typeV != NULL)
    {
      KjNode* typeP = kjLookup(eP, "type");

      if (typeP == NULL)
        continue;

      // Simple OR: entity type (string or array) must contain at least one of typeV
      bool found = false;

      if (typeP->type == KjString)
        found = matchStringV(typeP->value.s, filterP->typeV);
      else if (typeP->type == KjArray)
      {
        for (KjNode* elemP = typeP->value.firstChildP; elemP != NULL && !found; elemP = elemP->next)
        {
          if (elemP->type == KjString)
            found = matchStringV(elemP->value.s, filterP->typeV);
        }
      }

      if (!found)
        continue;
    }

    //
    // Filter by scope (scopeQ)
    //
    if (filterP != NULL && filterP->scopeExpr != NULL)
    {
      KjNode* scopeP = kjLookup(eP, LD_VOCAB_SCOPE);

      if (!ldEntityMatchScope(scopeP, filterP->scopeExpr))
        continue;
    }

    //
    // Filter by q expression
    //
    if (filterP != NULL && filterP->qExpr != NULL)
    {
      if (!ldEntityMatchQ(eP, filterP->qExpr))
        continue;
    }

    //
    // Geo-query filter (GEOS) — geoDistance is set for near queries, -1 otherwise
    //
    double geoDistance = -1;
    if (filterP != NULL && filterP->geoRel != NULL)
    {
      if (!corDbGeoMatch(eP, filterP, &geoDistance))
        continue;
    }
    else if (distFilterP != NULL)
    {
      // § 7.6.2.2 sort-by-distance (no filtering): distance from orderFrom to the
      // named GeoProperty. A missing/non-Point GeoProperty leaves geoDistance -1
      // (the entity is kept but ranks last).
      corDbGeoMatch(eP, distFilterP, &geoDistance);
    }

    cands[nCand].eP   = eP;
    cands[nCand].dist = geoDistance;
    nCand++;
  }

  if (filterP != NULL && filterP->count)
    filterP->totalCount = nCand;

  //
  // A near query returns the matches nearest-first (mongo $geoNear does the
  // same). Sort by distance before paginating so offset/limit page over the
  // distance order, not the store order. Other georels keep store order.
  //
  if (filterP != NULL && filterP->geoRel != NULL && filterP->geoRel->rel == LdGeoNear)
    qsort(cands, nCand, sizeof(GeoCand), geoCandCmp);
  else if (filterP != NULL && filterP->distGeoproperty != NULL)
  {
    // § 7.6.2.2 sort-by-distance — geo-bearing entities nearest/farthest first,
    // non-geo entities last. Paginate over this order, like the near path.
    distDescCand = filterP->distDesc;
    qsort(cands, nCand, sizeof(GeoCand), distCandCmp);
  }

  //
  // Paginate (offset/limit) and render. limit == 0 means count-only.
  //
  if (limit > 0)
  {
    for (int i = offset; i < nCand && (i - offset) < limit; i++)
    {
      KjNode* cloneP = kjClone(corRest.kjsonP, cands[i].eP);

      if (cands[i].dist >= 0)
        kjChildAdd(cloneP, kjFloat(corRest.kjsonP, "geoDistance", cands[i].dist));

      kjChildAdd(arrayP, cloneP);
    }
  }

  *arrayPP = arrayP;
  return DB_OK;
}

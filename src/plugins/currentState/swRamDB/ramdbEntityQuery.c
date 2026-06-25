//
// FILE            ramdbEntityQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <regex.h>                                     // regcomp, regexec, regfree
#include <stdlib.h>                                    // strtod, qsort
#include <string.h>                                    // strcmp

#include "kalloc/kaAlloc.h"                           // kaAlloc
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjArray, kjClone, kjFloat, kjChildAdd
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjLookup.h"                           // kjLookup
#include "swRest/SwRestState.h"                       // swRest
#include "swNgsild/LdQ.h"                              // LdQNode
#include "swNgsild/LdVocab.h"                         // LD_VOCAB_SCOPE
#include "swNgsild/LdScopeExpr.h"                     // LdScopeExpr
#include "swNgsild/LdGeoRel.h"                         // LdGeoRel, LdGeoNear
#include "swNgsild/ldEntityMatch.h"                    // ldEntityMatchType, ldEntityMatchScope, ldEntityMatchQ

#include "db/DbDriver.h"                              // DB_OK, Tenant
#include "currentState/swRamDB/ramdbStore.h"          // ramdbEntities
#include "currentState/swRamDB/ramdbGeoMatch.h"       // ramdbGeoMatch
#include "currentState/swRamDB/ramdbEntityQuery.h"    // Own interface



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



// -----------------------------------------------------------------------------
//
// ramdbEntityQuery -
//
int ramdbEntityQuery(Tenant* tenantP, DbQueryFilter* filterP, KjNode** arrayPP)
{
  KjNode* entities = ramdbEntities(tenantP);
  KjNode* arrayP   = kjArray(swRest.kjsonP, NULL);
  int     limit    = (filterP != NULL) ? filterP->limit  : 0;
  int     offset   = (filterP != NULL) ? filterP->offset : 0;

  //
  // Count the store so the candidate array can be sized up front.
  //
  int total = 0;
  for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next) total++;

  GeoCand* cands = (GeoCand*) kaAlloc(&swRest.kalloc, sizeof(GeoCand) * (total > 0 ? total : 1));
  int      nCand = 0;

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
      if (!ramdbGeoMatch(eP, filterP, &geoDistance))
        continue;
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

  //
  // Paginate (offset/limit) and render. limit == 0 means count-only.
  //
  if (limit > 0)
  {
    for (int i = offset; i < nCand && (i - offset) < limit; i++)
    {
      KjNode* cloneP = kjClone(swRest.kjsonP, cands[i].eP);

      if (cands[i].dist >= 0)
        kjChildAdd(cloneP, kjFloat(swRest.kjsonP, "geoDistance", cands[i].dist));

      kjChildAdd(arrayP, cloneP);
    }
  }

  *arrayPP = arrayP;
  return DB_OK;
}

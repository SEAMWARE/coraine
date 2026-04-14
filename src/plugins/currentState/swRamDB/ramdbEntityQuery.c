//
// FILE            ramdbEntityQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <regex.h>                                     // regcomp, regexec, regfree
#include <stdlib.h>                                    // strtod
#include <string.h>                                    // strcmp

#include "kbase/kStringArrayLookup.h"                 // kStringArrayLookup
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjArray
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjLookup.h"                           // kjLookup
#include "swRest/SwRestState.h"                       // swRest
#include "swNgsild/LdQ.h"                              // LdQNode
#include "swNgsild/LdVocab.h"                         // LD_VOCAB_SCOPE
#include "swNgsild/LdScopeExpr.h"                     // LdScopeExpr
#include "swNgsild/ldScopeMatch.h"                     // ldScopePatternMatch
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
// ramdbEntityQuery -
//
int ramdbEntityQuery(Tenant* tenantP, DbQueryFilter* filterP, KjNode** arrayPP)
{
  KjNode* entities = ramdbEntities(tenantP);
  KjNode* arrayP   = kjArray(swRest.kjsonP, NULL);
  int     matched  = 0;
  int     skipped  = 0;
  int     added    = 0;
  int     limit    = (filterP != NULL) ? filterP->limit  : 0;
  int     offset   = (filterP != NULL) ? filterP->offset : 0;

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
    // Geo-query filter (GEOS)
    //
    double geoDistance = -1;
    if (filterP != NULL && filterP->geoRel != NULL)
    {
      if (!ramdbGeoMatch(eP, filterP, &geoDistance))
        continue;
    }

    matched++;

    // Apply offset
    if (skipped < offset)
    {
      skipped++;
      continue;
    }

    // Apply limit (0 means no results wanted -- count-only mode)
    if (limit == 0 || added >= limit)
      continue;

    KjNode* cloneP = kjClone(swRest.kjsonP, eP);

    // Add geoDistance for near queries
    if (geoDistance >= 0)
    {
      KjNode* distP = kjFloat(swRest.kjsonP, "geoDistance", geoDistance);
      kjChildAdd(cloneP, distP);
    }

    kjChildAdd(arrayP, cloneP);
    added++;
  }

  if (filterP != NULL && filterP->count)
    filterP->totalCount = matched;

  *arrayPP = arrayP;
  return DB_OK;
}

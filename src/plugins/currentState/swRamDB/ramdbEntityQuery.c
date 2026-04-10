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

#include "db/DbDriver.h"                              // DB_OK, Tenant
#include "currentState/swRamDB/ramdbStore.h"          // ramdbEntities
#include "currentState/swRamDB/ramdbEntityQuery.h"    // Own interface



// -----------------------------------------------------------------------------
//
// entityHasType - check if entity has a specific type (handles both string and array "type")
//
static bool entityHasType(KjNode* typeP, const char* uri)
{
  if (typeP == NULL)
    return false;

  if (typeP->type == KjString)
    return (strcmp(typeP->value.s, uri) == 0);

  if (typeP->type == KjArray)
  {
    for (KjNode* elemP = typeP->value.firstChildP; elemP != NULL; elemP = elemP->next)
    {
      if (elemP->type == KjString && strcmp(elemP->value.s, uri) == 0)
        return true;
    }
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// matchTypeExpr - check if entity matches a type selection expression
//
static bool matchTypeExpr(KjNode* typeP, LdTypeExpr* expr)
{
  for (int gix = 0; gix < expr->groupCount; gix++)
  {
    LdTypeGroup* grp     = &expr->groupV[gix];
    bool         allMatch = true;

    for (int tix = 0; tix < grp->count; tix++)
    {
      if (!entityHasType(typeP, grp->typeV[tix]))
      {
        allMatch = false;
        break;
      }
    }

    if (allMatch)
      return true;  // OR -- any group matching is enough
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// entityScopeMatchesPattern - check if an entity's scope field matches one pattern
//
// Scope can be a string or an array of strings.
// For arrays, a match against ANY element is sufficient.
//
static bool entityScopeMatchesPattern(KjNode* scopeP, const char* pattern)
{
  if (scopeP == NULL)
    return false;

  if (scopeP->type == KjString)
    return ldScopePatternMatch(pattern, scopeP->value.s);

  if (scopeP->type == KjArray)
  {
    for (KjNode* elemP = scopeP->value.firstChildP; elemP != NULL; elemP = elemP->next)
    {
      if (elemP->type == KjString && ldScopePatternMatch(pattern, elemP->value.s))
        return true;
    }
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// matchScopeExpr - check if entity matches a scopeQ expression
//
static bool matchScopeExpr(KjNode* scopeP, LdScopeExpr* expr)
{
  for (int gix = 0; gix < expr->groupCount; gix++)
  {
    LdScopeGroup* grp      = &expr->groupV[gix];
    bool          allMatch = true;

    for (int six = 0; six < grp->count; six++)
    {
      if (!entityScopeMatchesPattern(scopeP, grp->scopeV[six]))
      {
        allMatch = false;
        break;
      }
    }

    if (allMatch)
      return true;  // OR -- any group matching is enough
  }

  return false;
}



// -----------------------------------------------------------------------------
//
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
// getAttrValue - find the "value" node for an attribute in an entity
//
// Entity-level temporal properties (createdAt, modifiedAt, expiresAt) are
// stored directly as children of the entity node.  Regular attributes are
// wrapped: entity -> attr-wrapper (object) -> instance (object) -> "value".
//
static KjNode* getAttrValue(KjNode* entityP, const char* attrName)
{
  // Entity-level temporal properties (not wrapped)
  if (strcmp(attrName, LD_VOCAB_CREATED_AT)  == 0 ||
      strcmp(attrName, LD_VOCAB_MODIFIED_AT) == 0 ||
      strcmp(attrName, LD_VOCAB_EXPIRES_AT)  == 0)
  {
    return kjLookup(entityP, attrName);
  }

  // Regular attribute: find wrapper, get default instance, get "value"
  KjNode* wrapperP = kjLookup(entityP, attrName);
  if (wrapperP == NULL || wrapperP->type != KjObject)
    return NULL;

  // Get first instance (usually "@none")
  KjNode* instP = wrapperP->value.firstChildP;
  if (instP == NULL || instP->type != KjObject)
    return NULL;

  // Get "value" from the instance
  return kjLookup(instP, "value");
}



// -----------------------------------------------------------------------------
//
// matchTerm - evaluate a single LdQTerm against an entity
//
static bool matchTerm(KjNode* entityP, LdQTerm* term)
{
  // Existence check (no operator)
  if (term->op == LdQExists)
  {
    KjNode* wrapperP = kjLookup(entityP, term->attr);
    return (wrapperP != NULL);
  }

  KjNode* valueP = getAttrValue(entityP, term->attr);
  if (valueP == NULL)
    return false;

  // Get the numeric value from the node for comparison
  double entityNum = 0;
  bool   isNum     = false;

  if (valueP->type == KjInt)        { entityNum = (double) valueP->value.i; isNum = true; }
  else if (valueP->type == KjFloat) { entityNum = valueP->value.f;          isNum = true; }

  switch (term->valueType)
  {
  case LdQNumber:
    if (!isNum) return false;
    switch (term->op)
    {
    case LdQEqual:     return entityNum == term->value.n;
    case LdQUnequal:   return entityNum != term->value.n;
    case LdQGreater:   return entityNum >  term->value.n;
    case LdQLess:      return entityNum <  term->value.n;
    case LdQGreaterEq: return entityNum >= term->value.n;
    case LdQLessEq:    return entityNum <= term->value.n;
    default:           return false;
    }

  case LdQString:
    if (term->op == LdQPattern || term->op == LdQNotPattern)
    {
      if (valueP->type != KjString) return false;
      regex_t re;
      if (regcomp(&re, term->value.s, REG_EXTENDED | REG_NOSUB) != 0)
        return false;
      bool m = (regexec(&re, valueP->value.s, 0, NULL, 0) == 0);
      regfree(&re);
      return (term->op == LdQPattern) ? m : !m;
    }
    if (valueP->type != KjString) return false;
    {
      int cmp = strcmp(valueP->value.s, term->value.s);
      switch (term->op)
      {
      case LdQEqual:     return cmp == 0;
      case LdQUnequal:   return cmp != 0;
      case LdQGreater:   return cmp >  0;
      case LdQLess:      return cmp <  0;
      case LdQGreaterEq: return cmp >= 0;
      case LdQLessEq:    return cmp <= 0;
      default:           return false;
      }
    }

  case LdQBool:
    if (valueP->type != KjBoolean) return false;
    {
      bool entityBool = valueP->value.b;
      switch (term->op)
      {
      case LdQEqual:   return entityBool == term->value.b;
      case LdQUnequal: return entityBool != term->value.b;
      default:         return false;
      }
    }

  case LdQDateTime:
    // Temporal properties are stored as KjInt nanoseconds
    if (valueP->type != KjInt) return false;
    {
      long long entityNs = valueP->value.i;
      long long queryNs  = term->value.ns;
      switch (term->op)
      {
      case LdQEqual:     return entityNs == queryNs;
      case LdQUnequal:   return entityNs != queryNs;
      case LdQGreater:   return entityNs >  queryNs;
      case LdQLess:      return entityNs <  queryNs;
      case LdQGreaterEq: return entityNs >= queryNs;
      case LdQLessEq:    return entityNs <= queryNs;
      default:           return false;
      }
    }

  case LdQNoValue:
    // Pattern matching handled via regex (op is LdQPattern/LdQNotPattern)
    if (term->op == LdQPattern && valueP->type == KjString)
    {
      regex_t re;
      if (regcomp(&re, term->value.s, REG_EXTENDED | REG_NOSUB) == 0)
      {
        bool m = (regexec(&re, valueP->value.s, 0, NULL, 0) == 0);
        regfree(&re);
        return m;
      }
    }
    else if (term->op == LdQNotPattern && valueP->type == KjString)
    {
      regex_t re;
      if (regcomp(&re, term->value.s, REG_EXTENDED | REG_NOSUB) == 0)
      {
        bool m = (regexec(&re, valueP->value.s, 0, NULL, 0) != 0);
        regfree(&re);
        return m;
      }
    }
    return false;

  case LdQRange:
    if (!isNum) return false;
    if (term->op == LdQEqual)
      return entityNum >= term->value.numRange.lo && entityNum <= term->value.numRange.hi;
    else if (term->op == LdQUnequal)
      return entityNum < term->value.numRange.lo || entityNum > term->value.numRange.hi;
    return false;

  case LdQValueList:
    // Check if entity value matches any value in the list
    for (int i = 0; i < term->value.list.count; i++)
    {
      if (term->value.list.itemType == LdQNumber && isNum)
      {
        double listNum = strtod(term->value.list.values[i], NULL);
        if (entityNum == listNum)
          return (term->op == LdQEqual);
      }
      else if (term->value.list.itemType == LdQString && valueP->type == KjString)
      {
        if (strcmp(valueP->value.s, term->value.list.values[i]) == 0)
          return (term->op == LdQEqual);
      }
    }
    return (term->op == LdQUnequal);  // none matched -- unequal is true

  default:
    return false;
  }
}



// -----------------------------------------------------------------------------
//
// matchQExpr - recursively evaluate a q-filter expression tree against an entity
//
static bool matchQExpr(KjNode* entityP, LdQNode* node)
{
  if (node == NULL)
    return true;

  if (node->type == LdQTermNode)
    return matchTerm(entityP, &node->term);

  if (node->type == LdQAndNode)
  {
    for (int i = 0; i < node->group.count; i++)
    {
      if (!matchQExpr(entityP, node->group.childV[i]))
        return false;
    }
    return true;
  }

  if (node->type == LdQOrNode)
  {
    for (int i = 0; i < node->group.count; i++)
    {
      if (matchQExpr(entityP, node->group.childV[i]))
        return true;
    }
    return false;
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

      if (!matchTypeExpr(typeP, filterP->typeExpr))
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

      if (!matchScopeExpr(scopeP, filterP->scopeExpr))
        continue;
    }

    //
    // Filter by q expression
    //
    if (filterP != NULL && filterP->qExpr != NULL)
    {
      if (!matchQExpr(eP, filterP->qExpr))
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
    kjChildAdd(arrayP, cloneP);
    added++;
  }

  if (filterP != NULL && filterP->count)
    filterP->totalCount = matched;

  *arrayPP = arrayP;
  return DB_OK;
}

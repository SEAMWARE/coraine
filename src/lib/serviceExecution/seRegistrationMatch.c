//
// FILE            seRegistrationMatch.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                   // strcmp, strchr, strlen, strncmp
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
#include "corNgsild/ldEntityMatch.h"                  // ldEntityMatchQ, ldEntityMatchScope
#include "corNgsild/LdScopeExpr.h"                    // ldScopeExprParse

#include "db/DbDriver.h"                              // db, DB_OK
#include "db/Tenant.h"                                // Tenant

#include "serviceExecution/seRegistrationMatch.h"     // Own interface



// -----------------------------------------------------------------------------
//
// str - a member's string value, or NULL
//
static const char* str(CorNode* objectP, const char* name)
{
  CorNode* nodeP = (objectP != NULL) ? corTreeLookup(objectP, name) : NULL;

  return ((nodeP != NULL) && (nodeP->type == CorString)) ? nodeP->value.s : NULL;
}



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

  CorNode* scopeQP = corTreeLookup(regP, "scopeQ");

  if ((scopeQP != NULL) && (scopeQP->type == CorString))
  {
    LdScopeExpr* scopeExprP = ldScopeExprParse(scopeQP->value.s, &corRest.kalloc);

    if ((scopeExprP == NULL) || (ldEntityMatchScope(corTreeLookup(entityP, "scope"), scopeExprP) == false))
      return false;
  }

  CorNode* geoQP = corTreeLookup(regP, "geoQ");

  if ((geoQP != NULL) && (geoQP->type == CorObject) && (geoQMatches(geoQP, entityP) == false))
    return false;

  return true;
}



// -----------------------------------------------------------------------------
//
// regexMatches - does the idPattern match the id? An idPattern that does not compile: it might (conservative)
//
static bool regexMatches(const char* pattern, const char* id)
{
  regex_t re;

  if (regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB) != 0)
    return true;

  bool match = (regexec(&re, id, 0, NULL, 0) == 0);

  regfree(&re);
  return match;
}



// -----------------------------------------------------------------------------
//
// literalPrefix - the literal text every id an anchored idPattern matches starts with; NULL: none known
//
// "^urn:Robot:.*" -> "urn:Robot:"; "^urn:A\.b" -> "urn:A.b"; a character made optional by the quantifier
// after it ('*', '?', '{') ends the prefix before it. A pattern not anchored with '^', or with a '|'
// anywhere (an alternative may start elsewhere): NULL.
//
static const char* literalPrefix(const char* pattern, char* buf, int bufSize)
{
  if ((pattern[0] != '^') || (strchr(pattern, '|') != NULL))
    return NULL;

  const char* cP = &pattern[1];
  int         n  = 0;

  while ((*cP != 0) && (n < bufSize - 1))
  {
    char        lit;
    const char* nextP;

    if ((cP[0] == '\\') && (cP[1] != 0) && (strchr(".[]()*+?{}^$\\/-", cP[1]) != NULL))
    {
      lit   = cP[1];
      nextP = &cP[2];
    }
    else if (strchr(".[]()*+?{}^$\\", cP[0]) != NULL)
      break;
    else
    {
      lit   = cP[0];
      nextP = &cP[1];
    }

    if ((*nextP == '*') || (*nextP == '?') || (*nextP == '{'))
      break;                                          // optional: not part of every id

    buf[n++] = lit;
    cP       = nextP;
  }

  buf[n] = 0;
  return buf;
}



// -----------------------------------------------------------------------------
//
// idPatternsOverlap - could one id match both? Yes, unless both are anchored literal prefixes neither of
// which starts the other
//
static bool idPatternsOverlap(const char* pattern1, const char* pattern2)
{
  char        buf1[256];
  char        buf2[256];
  const char* prefix1 = literalPrefix(pattern1, buf1, sizeof(buf1));
  const char* prefix2 = literalPrefix(pattern2, buf2, sizeof(buf2));

  if ((prefix1 == NULL) || (prefix2 == NULL))
    return true;

  size_t len1 = strlen(prefix1);
  size_t len2 = strlen(prefix2);

  return strncmp(prefix1, prefix2, (len1 < len2) ? len1 : len2) == 0;
}



// -----------------------------------------------------------------------------
//
// selectorsOverlap - could one entity be selected by both members of 'entities'?
//
// The types (stored expanded) must be the same; then: two ids - the same id; an id and an idPattern -
// the pattern matches the id; two idPatterns - idPatternsOverlap; a selector with no id - any id.
//
static bool selectorsOverlap(CorNode* sel1P, CorNode* sel2P)
{
  const char* type1 = str(sel1P, "type");
  const char* type2 = str(sel2P, "type");

  if ((type1 != NULL) && (type2 != NULL) && (strcmp(type1, type2) != 0))
    return false;

  const char* id1  = str(sel1P, "id");
  const char* id2  = str(sel2P, "id");
  const char* pat1 = str(sel1P, "idPattern");
  const char* pat2 = str(sel2P, "idPattern");

  if ((id1 != NULL) && (id2 != NULL))
    return strcmp(id1, id2) == 0;

  if ((id1 != NULL) && (pat2 != NULL))
    return regexMatches(pat2, id1);

  if ((pat1 != NULL) && (id2 != NULL))
    return regexMatches(pat1, id2);

  if ((pat1 != NULL) && (pat2 != NULL))
    return idPatternsOverlap(pat1, pat2);

  return true;
}



// -----------------------------------------------------------------------------
//
// seRegistrationNameConflict -
//
const char* seRegistrationNameConflict(CorNode* regP)
{
  const char* ownId       = str(regP, "id");
  const char* serviceName = str(corTreeLookup(regP, "serviceInformation"), "serviceName");
  CorNode*    entitiesP   = corTreeLookup(regP, "entities");
  CorNode*    regsP       = NULL;
  const char* conflictId  = NULL;

  if ((serviceName == NULL) || (entitiesP == NULL) || (entitiesP->type != CorArray))
    return NULL;

  if ((db.docQuery == NULL) || (db.docQuery((Tenant*) corNgsild.tenantP, "serviceRegistrations", &regsP) != DB_OK) || (regsP == NULL))
    return NULL;

  for (CorNode* otherP = regsP->value.head; otherP != NULL; otherP = otherP->next)
  {
    const char* otherId   = str(otherP, "id");
    const char* otherName = str(corTreeLookup(otherP, "serviceInformation"), "serviceName");
    CorNode*    otherEntP = corTreeLookup(otherP, "entities");

    if ((otherName == NULL) || (strcmp(otherName, serviceName) != 0) || (otherEntP == NULL) || (otherEntP->type != CorArray))
      continue;

    if ((ownId != NULL) && (otherId != NULL) && (strcmp(ownId, otherId) == 0))
      continue;                                       // itself (a PATCH), or its own id taken (the create's 409)

    bool overlap = false;

    for (CorNode* selP = entitiesP->value.head; (selP != NULL) && (overlap == false); selP = selP->next)
    {
      for (CorNode* otherSelP = otherEntP->value.head; (otherSelP != NULL) && (overlap == false); otherSelP = otherSelP->next)
        overlap = selectorsOverlap(selP, otherSelP);
    }

    if (overlap == false)
      continue;

    if (otherId == NULL)
      otherId = "";

    if ((conflictId == NULL) || (strcmp(otherId, conflictId) < 0))
      conflictId = otherId;                           // of several, the first by id - one answer whatever the database's order
  }

  return conflictId;
}

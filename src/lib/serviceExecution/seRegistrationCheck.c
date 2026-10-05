//
// FILE            seRegistrationCheck.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                   // strcmp
#include <regex.h>                                    // regcomp, regfree

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corRest/CorRestState.h"                     // corRest
#include "corNgsild/ldError.h"                        // ldError
#include "corNgsild/corNgsild.h"                      // LD_ERROR_BAD_REQUEST_DATA
#include "corNgsild/ldCheckUri.h"                     // ldUriValid
#include "corNgsild/ldCheckGeo.h"                     // ldCheckGeoQuery
#include "corNgsild/ldQParse.h"                       // ldQParse
#include "corNgsild/ldIso8601Duration.h"              // ldIso8601DurationParseNs
#include "corNgsild/CorNgsild.h"                      // corNgsild
#include "corJsonld/corLdCompact.h"                   // corLdCompact

#include "serviceExecution/seRegistrationCheck.h"     // Own interface



// -----------------------------------------------------------------------------
//
// bad - a 400 BadRequestData, always false
//
#define bad(...)  (ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", __VA_ARGS__), false)



// -----------------------------------------------------------------------------
//
// shortName - a member's name as the client wrote it: an unknown member arrives expanded
//
static const char* shortName(const char* name)
{
  return corLdCompact(corNgsild.contextP, name);
}



// -----------------------------------------------------------------------------
//
// isString -
//
static bool isString(CorNode* nodeP)
{
  return (nodeP != NULL) && (nodeP->type == CorString) && (nodeP->value.s[0] != 0);
}



// -----------------------------------------------------------------------------
//
// entitiesCheck - a non-empty array of { type, id | idPattern }
//
static bool entitiesCheck(CorNode* entitiesP)
{
  if ((entitiesP == NULL) || (entitiesP->type != CorArray) || (entitiesP->value.head == NULL))
    return bad("a Service Registration needs 'entities': a non-empty array of { type, id | idPattern }");

  for (CorNode* eP = entitiesP->value.head; eP != NULL; eP = eP->next)
  {
    if (eP->type != CorObject)
      return bad("a member of 'entities' must be an object");

    CorNode* typeP      = NULL;
    CorNode* idP        = NULL;
    CorNode* idPatternP = NULL;

    for (CorNode* mP = eP->value.head; mP != NULL; mP = mP->next)
    {
      if      (strcmp(mP->name, "type")      == 0) typeP      = mP;
      else if (strcmp(mP->name, "id")        == 0) idP        = mP;
      else if (strcmp(mP->name, "idPattern") == 0) idPatternP = mP;
      else
        return bad("'entities' member: '%s' is not one of type, id, idPattern", shortName(mP->name));
    }

    if (isString(typeP) == false)
      return bad("a member of 'entities' needs a 'type'");

    if ((idP != NULL) && (idPatternP != NULL))
      return bad("a member of 'entities' has an 'id' or an 'idPattern', not both");

    if ((idP != NULL) && ((isString(idP) == false) || (ldUriValid(idP->value.s) == false)))
      return bad("'entities' member: 'id' must be a URI");

    if (idPatternP != NULL)
    {
      regex_t re;

      if ((isString(idPatternP) == false) || (regcomp(&re, idPatternP->value.s, REG_EXTENDED | REG_NOSUB) != 0))
        return bad("'entities' member: 'idPattern' must be a regular expression");

      regfree(&re);
    }
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// serviceInformationCheck -
//
static bool serviceInformationCheck(CorNode* siP)
{
  if ((siP == NULL) || (siP->type != CorObject))
    return bad("a Service Registration needs 'serviceInformation': an object naming the service");

  CorNode* nameP = NULL;

  for (CorNode* mP = siP->value.head; mP != NULL; mP = mP->next)
  {
    if (strcmp(mP->name, "serviceName") == 0)
      nameP = mP;
    else if ((strcmp(mP->name, "title") == 0) || (strcmp(mP->name, "description") == 0))
    {
      if (mP->type != CorString)
        return bad("serviceInformation: '%s' must be a string", mP->name);
    }
    else if (strcmp(mP->name, "mode") == 0)
    {
      if ((mP->type != CorString) || ((strcmp(mP->value.s, "synchronous") != 0) && (strcmp(mP->value.s, "asynchronous") != 0)))
        return bad("serviceInformation: 'mode' is \"synchronous\" or \"asynchronous\"");
    }
    else if ((strcmp(mP->name, "inputSchema") == 0) || (strcmp(mP->name, "outputSchema") == 0))
    {
      if ((mP->type != CorObject) && (mP->type != CorBoolean))
        return bad("serviceInformation: '%s' must be a JSON Schema", mP->name);
    }
    else if ((strcmp(shortName(mP->name), "input") == 0) || (strcmp(shortName(mP->name), "output") == 0))
      return bad("serviceInformation: '%s' - a service's JSON Schemas are 'inputSchema' and 'outputSchema'", shortName(mP->name));
    else
      return bad("serviceInformation: '%s' is not a member of a service's information", shortName(mP->name));
  }

  if (isString(nameP) == false)
    return bad("serviceInformation needs a 'serviceName'");

  return true;
}



// -----------------------------------------------------------------------------
//
// seRegistrationCheck -
//
bool seRegistrationCheck(CorNode* regP)
{
  if ((regP == NULL) || (regP->type != CorObject))
    return bad("a Service Registration is a JSON object");

  CorNode* typeP     = NULL;
  CorNode* endpointP = NULL;
  CorNode* entitiesP = NULL;
  CorNode* siP       = NULL;

  for (CorNode* mP = regP->value.head; mP != NULL; mP = mP->next)
  {
    if      (strcmp(mP->name, "type")               == 0) typeP     = mP;
    else if (strcmp(mP->name, "endpoint")           == 0) endpointP = mP;
    else if (strcmp(mP->name, "entities")           == 0) entitiesP = mP;
    else if (strcmp(mP->name, "serviceInformation") == 0) siP       = mP;
    else if (strcmp(mP->name, "id") == 0)
    {
      if ((isString(mP) == false) || (ldUriValid(mP->value.s) == false))
        return bad("'id' must be a URI");
    }
    else if (strcmp(mP->name, "q") == 0)
    {
      if ((isString(mP) == false) || (ldQParse(mP->value.s, &corRest.kalloc) == NULL))
        return bad("'q' must be a valid NGSI-LD query");
    }
    else if (strcmp(mP->name, "geoQ") == 0)
    {
      CorNode* geometryP    = (mP->type == CorObject) ? corTreeLookup(mP, "geometry")    : NULL;
      CorNode* coordinatesP = (mP->type == CorObject) ? corTreeLookup(mP, "coordinates") : NULL;
      CorNode* georelP      = (mP->type == CorObject) ? corTreeLookup(mP, "georel")      : NULL;

      if ((isString(geometryP) == false) || (coordinatesP == NULL) || (isString(georelP) == false))
        return bad("'geoQ' needs geometry, coordinates and georel");
    }
    else if (strcmp(mP->name, "executionTimeout") == 0)
    {
      if ((isString(mP) == false) || (ldIso8601DurationParseNs(mP->value.s) <= 0))
        return bad("'executionTimeout' must be an ISO 8601 duration");
    }
    else if ((strcmp(mP->name, "createdAt") == 0) || (strcmp(mP->name, "modifiedAt") == 0))
      continue;                                       // the broker's own, on a stored registration
    else if (strcmp(mP->name, "mode") == 0)
      return bad("'mode' belongs in serviceInformation");
    else
      return bad("'%s' is not a member of a Service Registration", shortName(mP->name));
  }

  if ((isString(typeP) == false) || (strcmp(typeP->value.s, "ServiceRegistration") != 0))
    return bad("a Service Registration's 'type' is \"ServiceRegistration\"");

  if ((isString(endpointP) == false) || (ldUriValid(endpointP->value.s) == false))
    return bad("a Service Registration needs an 'endpoint': the URI of the service's executor");

  return entitiesCheck(entitiesP) && serviceInformationCheck(siP);
}

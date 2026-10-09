//
// FILE            seCombined.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdio.h>                                    // snprintf
#include <string.h>                                   // strcmp, strlen, memset
#include <time.h>                                     // clock_gettime

#include "corLog/corLog.h"                            // COR_W
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeArray, corTreeString, corTreeChildAdd, ...
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corJson/corJsonRender.h"                    // corJsonFastRender
#include "corJson/corJsonRenderSize.h"                // corJsonFastRenderSize
#include "corJsonld/corLdCompact.h"                   // corLdCompact
#include "corRest/CorRestState.h"                     // corRest
#include "corRest/corRestOutHeader.h"                 // corRestOutHeaderAdd
#include "corNgsild/CorNgsild.h"                      // corNgsild
#include "corNgsild/ldError.h"                        // ldError
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*
#include "corNgsild/ldIdGenerate.h"                   // ldIdGenerate
#include "corNgsild/ldSysTimestamp.h"                 // ldSysTimestampCreate, ldSysTimestampToIso
#include "corNgsild/ldQParse.h"                       // ldQParse
#include "corNgsild/LdGeoRel.h"                       // ldGeoRelParse
#include "corNgsild/LdScopeExpr.h"                    // ldScopeExprParse
#include "corNgsild/ldEntityMatch.h"                  // ldEntityMatchScope
#include "corJsonld/corLdExpand.h"                    // corLdExpand

#include "db/DbDriver.h"                              // db, DB_*
#include "db/DbQueryFilter.h"                         // DbQueryFilter
#include "db/Tenant.h"                                // Tenant
#include "serviceExecution/seExecution.h"             // seExecutionBuild, seExecutionStart, seExecutionStore, ...
#include "serviceExecution/seCombined.h"              // Own interface



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
// nowNs -
//
static int64_t nowNs(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}



// -----------------------------------------------------------------------------
//
// terminal -
//
static bool terminal(const char* status)
{
  return (status != NULL) && ((strcmp(status, "completed") == 0) || (strcmp(status, "failed") == 0) || (strcmp(status, "cancelled") == 0));
}



// -----------------------------------------------------------------------------
//
// member - set (replace or add)
//
static void member(CorNode* objectP, CorNode* nodeP)
{
  CorNode* oldP = corTreeLookup(objectP, nodeP->name);

  if (oldP != NULL)
    corTreeChildRemove(objectP, oldP);

  corTreeChildAdd(objectP, nodeP);
}



// -----------------------------------------------------------------------------
//
// ended - a parent's terminal status, with executionEndedAt
//
static void ended(CorNode* execP, const char* status)
{
  char    buf[64];
  int64_t now = nowNs();

  member(execP, corTreeString(corRest.kallocP, "executionStatus", status));
  ldSysTimestampToIso((long long) now, buf, sizeof(buf));
  member(execP, corTreeString(corRest.kallocP, "executionEndedAt", buf));
  member(execP, corTreeInteger(corRest.kallocP, "_endedNs", now));
}



// -----------------------------------------------------------------------------
//
// methodOk - a combinationMethod: true for one that is built; false with a 400 or a 501 raised
//
static bool methodOk(CorNode* methodP)
{
  const char* m = ((methodP != NULL) && (methodP->type == CorString)) ? methodP->value.s : NULL;

  if (m == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "a 'combinationMethod' is needed: sequential or parallel");
    return false;
  }

  if ((strcmp(m, "sequential") == 0) || (strcmp(m, "parallel") == 0))
    return true;

  if ((strcmp(m, "conditional") == 0) || (strcmp(m, "timetriggered") == 0) || (strcmp(m, "recurring") == 0))
  {
    ldError(501, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented", "combinationMethod '%s' is still to be specified (GR CIM-055 § 6.3.5.2)", m);
    return false;
  }

  ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "combinationMethod '%s' - it is sequential or parallel", m);
  return false;
}



// -----------------------------------------------------------------------------
//
// Built - the executions a creation builds, in memory, before any is stored
//
typedef struct Built
{
  CorNode* docV[256];
  int      docN;
} Built;



// -----------------------------------------------------------------------------
//
// buildRefused - a child refused: the request answered with why
//
static CorNode* buildRefused(int status, const char* why)
{
  if (status == 404)
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "%s", why);
  else if (status == 409)
    ldError(409, LD_ERROR_CONFLICT, "Conflict", "%s", why);
  else
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "%s", why);

  return NULL;
}



// -----------------------------------------------------------------------------
//
// buildSimple - one simple child
//
static CorNode* buildSimple(Built* bP, const char* entityId, const char* serviceName, CorNode* inputP, const char* entityType, const char* parentId)
{
  int   status = 0;
  char  why[512];

  if ((entityId == NULL) || (serviceName == NULL))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "a service execution names its 'entityId' and its 'serviceName'");
    return NULL;
  }

  if ((inputP != NULL) && (inputP->type != CorObject))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'executionInput' is a JSON object");
    return NULL;
  }

  CorNode* execP = seExecutionBuild(entityId, serviceName, inputP, NULL, &status, why, sizeof(why));

  if (execP == NULL)
    return buildRefused(status, why);

  if ((entityType != NULL) && (strcmp(str(execP, "entityType"), entityType) != 0))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "entity '%s' is not of the template's type '%s'", entityId, corLdCompact(corNgsild.contextP, entityType));
    return NULL;
  }

  if (bP->docN >= 255)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "too many service executions in one request (255 at most)");
    return NULL;
  }

  corTreeChildAdd(execP, corTreeString(corRest.kallocP, "_parent", parentId));
  bP->docV[bP->docN++] = execP;

  return execP;
}



// -----------------------------------------------------------------------------
//
// parentBuild - a combined or grouped execution, its children to come
//
static CorNode* parentBuild(Built* bP, const char* type, const char* id, const char* method, const char* parentId)
{
  CorNode* execP = corTreeObject(corRest.kallocP, NULL);

  if (bP->docN >= 255)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "too many service executions in one request (255 at most)");
    return NULL;
  }

  corTreeChildAdd(execP, corTreeString(corRest.kallocP, "id", (id != NULL) ? id : ldIdGenerate(&corRest.kalloc, type)));
  corTreeChildAdd(execP, corTreeString(corRest.kallocP, "type", type));
  corTreeChildAdd(execP, corTreeString(corRest.kallocP, "combinationMethod", method));
  corTreeChildAdd(execP, corTreeString(corRest.kallocP, "executionStatus", "pending"));
  corTreeChildAdd(execP, corTreeArray(corRest.kallocP, "_children"));

  if (parentId != NULL)
    corTreeChildAdd(execP, corTreeString(corRest.kallocP, "_parent", parentId));

  if ((corNgsild.contextP != NULL) && (corNgsild.contextP->url != NULL))
    corTreeChildAdd(execP, corTreeString(corRest.kallocP, "_context", corNgsild.contextP->url));

  ldSysTimestampCreate(execP);
  bP->docV[bP->docN++] = execP;

  return execP;
}

static void childAdd(CorNode* parentP, CorNode* childP)
{
  corTreeChildAdd(corTreeLookup(parentP, "_children"), corTreeString(corRest.kallocP, NULL, str(childP, "id")));
}



// -----------------------------------------------------------------------------
//
// The members of each object a creation is made of; any other is refused (400, naming it)
//
static const char* groupedMemberV[]  = { "type", "id", "combinationMethod", "serviceName", "entityType", "idPattern", "q", "geoQ", "scopeQ", "executionInput", "notification", NULL };
static const char* combinedMemberV[] = { "type", "id", "combinationMethod", "serviceExecutions", "notification", NULL };
static const char* nestedMemberV[]   = { "type", "id", "combinationMethod", "serviceExecutions", NULL };
static const char* fromTemplateV[]   = { "type", "id", "serviceTemplateId", "combinationMethod", "services", "notification", NULL };
static const char* simpleMemberV[]   = { "type", "entityId", "serviceName", "executionInput", NULL };
static const char* overrideMemberV[] = { "entityId", "serviceName", "executionInput", NULL };
static const char* templateMemberV[] = { "type", "id", "templateName", "combinationMethod", "services", "createdAt", "modifiedAt", NULL };  // the timestamps: the broker's own, on a stored one (PATCH)
static const char* nestedTmplV[]     = { "type", "combinationMethod", "services", NULL };
static const char* tmplServiceV[]    = { "serviceName", "entityType", "entityId", "executionInput", NULL };



// -----------------------------------------------------------------------------
//
// membersCheck - every member of objectP one of memberV; false: a 400 naming the first that is not
//
// 'what' names the object in the error ("a Grouped Service Execution", ...).
//
static bool membersCheck(CorNode* objectP, const char** memberV, const char* what)
{
  for (CorNode* mP = objectP->value.head; mP != NULL; mP = mP->next)
  {
    bool known = false;

    for (int ix = 0; (memberV[ix] != NULL) && (known == false); ix++)
      known = (strcmp(mP->name, memberV[ix]) == 0);

    if (known == true)
      continue;

    const char* name = corLdCompact(corNgsild.contextP, mP->name);

    if (strcmp(name, "input") == 0)
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'input' is not a member of %s - the service's input is 'executionInput'", what);
    else
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'%s' is not a member of %s", name, what);

    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// buildCombined - a CombinedServiceExecution and its serviceExecutions (nested ones included)
//
static CorNode* buildCombined(Built* bP, CorNode* nodeP, const char* parentId)
{
  CorNode* methodP = corTreeLookup(nodeP, "combinationMethod");
  CorNode* listP   = corTreeLookup(nodeP, "serviceExecutions");

  //
  // A nested one: no notification of its own - only the request's top object has one
  //
  if (membersCheck(nodeP, (parentId == NULL) ? combinedMemberV : nestedMemberV, (parentId == NULL) ? "a Combined Service Execution" : "a nested Combined Service Execution") == false)
    return NULL;

  if (methodOk(methodP) == false)
    return NULL;

  if ((listP == NULL) || (listP->type != CorArray) || (listP->value.head == NULL))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "a Combined Service Execution needs 'serviceExecutions': a non-empty array");
    return NULL;
  }

  CorNode* execP = parentBuild(bP, "CombinedServiceExecution", str(nodeP, "id"), methodP->value.s, parentId);

  if (execP == NULL)
    return NULL;

  for (CorNode* itemP = listP->value.head; itemP != NULL; itemP = itemP->next)
  {
    const char* type   = str(itemP, "type");
    CorNode*    childP = NULL;

    if ((itemP->type != CorObject) || (type == NULL))
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "a member of 'serviceExecutions' is a ServiceExecution or a CombinedServiceExecution");
      return NULL;
    }

    if (strcmp(type, "ServiceExecution") == 0)
    {
      if (corTreeLookup(itemP, "executionStatus") != NULL)
      {
        ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'executionStatus' is the broker's to set");
        return NULL;
      }

      if (membersCheck(itemP, simpleMemberV, "a Service Execution of 'serviceExecutions'") == false)
        return NULL;

      childP = buildSimple(bP, str(itemP, "entityId"), str(itemP, "serviceName"), corTreeLookup(itemP, "executionInput"), NULL, str(execP, "id"));
    }
    else if (strcmp(type, "CombinedServiceExecution") == 0)
      childP = buildCombined(bP, itemP, str(execP, "id"));
    else
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "a member of 'serviceExecutions' is a ServiceExecution or a CombinedServiceExecution, not a '%s'", type);
      return NULL;
    }

    if (childP == NULL)
      return NULL;

    childAdd(execP, childP);
  }

  return execP;
}



// -----------------------------------------------------------------------------
//
// buildFromTemplate - a template's services, each overridden by the request's member at the same place
//
// A member of the request's "services" (if there is one at that place) gives the entityId, and may give
// an executionInput that replaces the template's; its serviceName, if given, must be the template's.
// A nested template is taken as it is: the request's member at its place, if any, is an empty object.
// A member of "services" is an object of entityId, serviceName, executionInput; no more of them than the
// template has services.
//
static CorNode* buildFromTemplate(Built* bP, CorNode* templateP, CorNode* overridesP, const char* method, const char* id, const char* parentId)
{
  CorNode* servicesP = corTreeLookup(templateP, "services");

  if (overridesP != NULL)
  {
    int templateN = 0;
    int requestN  = 0;

    if (overridesP->type != CorArray)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'services' is an array: the template's services, by place");
      return NULL;
    }

    for (CorNode* sP = servicesP->value.head; sP != NULL; sP = sP->next)
      templateN++;

    for (CorNode* oP = overridesP->value.head; oP != NULL; oP = oP->next)
    {
      if (oP->type != CorObject)
      {
        ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "an entry of 'services' is an object: entityId, serviceName, executionInput");
        return NULL;
      }

      if (membersCheck(oP, overrideMemberV, "an entry of 'services'") == false)
        return NULL;

      requestN++;
    }

    if (requestN > templateN)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'services' has %d entries - the template has %d services", requestN, templateN);
      return NULL;
    }
  }

  CorNode* execP = parentBuild(bP, "CombinedServiceExecution", id, method, parentId);
  CorNode* overP = (overridesP != NULL) ? overridesP->value.head : NULL;

  if (execP == NULL)
    return NULL;

  for (CorNode* sP = servicesP->value.head; sP != NULL; sP = sP->next, overP = (overP != NULL) ? overP->next : NULL)
  {
    CorNode* childP = NULL;

    if ((str(sP, "type") != NULL) && (strcmp(str(sP, "type"), "CombinedServiceTemplate") == 0))
    {
      if ((overP != NULL) && (overP->value.head != NULL))
      {
        ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "the template's member at that place is a Combined Service Template - it is taken as it is: an empty object in 'services'");
        return NULL;
      }

      childP = buildFromTemplate(bP, sP, NULL, str(sP, "combinationMethod"), NULL, str(execP, "id"));
    }
    else
    {
      const char* serviceName = str(sP, "serviceName");
      const char* entityId    = (str(overP, "entityId") != NULL) ? str(overP, "entityId") : str(sP, "entityId");
      CorNode*    inputP      = ((overP != NULL) && (corTreeLookup(overP, "executionInput") != NULL)) ? corTreeLookup(overP, "executionInput") : corTreeLookup(sP, "executionInput");

      if ((str(overP, "serviceName") != NULL) && (strcmp(str(overP, "serviceName"), serviceName) != 0))
      {
        ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "the template's service at that place is '%s', not '%s'",
                corLdCompact(corNgsild.contextP, serviceName), corLdCompact(corNgsild.contextP, str(overP, "serviceName")));
        return NULL;
      }

      if (entityId == NULL)
      {
        ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "the template's service '%s' needs an 'entityId' in the request", corLdCompact(corNgsild.contextP, serviceName));
        return NULL;
      }

      childP = buildSimple(bP, entityId, serviceName, inputP, str(sP, "entityType"), str(execP, "id"));
    }

    if (childP == NULL)
      return NULL;

    childAdd(execP, childP);
  }

  return execP;
}



// -----------------------------------------------------------------------------
//
// buildGrouped - a GroupedServiceExecution: the service on every matching entity that offers it
//
// The selection: entityType, idPattern, q, geoQ and scopeQ (on the entity's scope, as a subscription's
// scopeQ) - any one of them, or several (all must hold).
//
static CorNode* buildGrouped(Built* bP, CorNode* bodyP)
{
  CorNode*    methodP     = corTreeLookup(bodyP, "combinationMethod");
  const char* serviceName = str(bodyP, "serviceName");
  const char* entityType  = str(bodyP, "entityType");
  const char* idPattern   = str(bodyP, "idPattern");
  const char* q           = str(bodyP, "q");
  CorNode*    geoQP       = corTreeLookup(bodyP, "geoQ");
  CorNode*    scopeQP     = corTreeLookup(bodyP, "scopeQ");
  CorNode*    inputP      = corTreeLookup(bodyP, "executionInput");

  if (membersCheck(bodyP, groupedMemberV, "a Grouped Service Execution") == false)
    return NULL;

  if (methodOk(methodP) == false)
    return NULL;

  if (serviceName == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "a Grouped Service Execution names its 'serviceName'");
    return NULL;
  }

  if ((entityType == NULL) && (idPattern == NULL) && (q == NULL) && (geoQP == NULL) && (scopeQP == NULL))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "a Grouped Service Execution selects its entities: entityType, idPattern, q, geoQ or scopeQ");
    return NULL;
  }

  if ((inputP != NULL) && (inputP->type != CorObject))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'executionInput' is a JSON object");
    return NULL;
  }

  //
  // The entities - the query's own filters, unpaged
  //
  DbQueryFilter filter;
  char*         typeV[2] = { (char*) entityType, NULL };

  memset(&filter, 0, sizeof(filter));
  filter.typeV     = (entityType != NULL) ? typeV : NULL;
  filter.idPattern = (char*) idPattern;
  filter.unpaged   = true;
  filter.limit     = 1000;

  if (q != NULL)
  {
    if ((filter.qExpr = ldQParse(q, &corRest.kalloc)) == NULL)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'q' is not a valid NGSI-LD query");
      return NULL;
    }
  }

  if (geoQP != NULL)
  {
    CorNode* coordsP = corTreeLookup(geoQP, "coordinates");

    filter.geoRel   = (str(geoQP, "georel") != NULL) ? ldGeoRelParse(str(geoQP, "georel"), &corRest.kalloc) : NULL;
    filter.geometry = (char*) str(geoQP, "geometry");

    if ((filter.geoRel == NULL) || (filter.geometry == NULL) || (coordsP == NULL))
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'geoQ' needs geometry, coordinates and georel");
      return NULL;
    }

    if (coordsP->type == CorString)
      filter.coordinates = coordsP->value.s;
    else
    {
      filter.coordinates = (char*) corAlloc(&corRest.kalloc, corJsonFastRenderSize(coordsP) + 1);
      corJsonFastRender(coordsP, filter.coordinates);
    }

    const char* geoproperty = (str(geoQP, "geoproperty") != NULL) ? str(geoQP, "geoproperty") : "location";

    filter.geoproperty = (char*) corLdExpand(corNgsild.contextP, geoproperty, &corRest.kalloc, NULL, NULL);
  }

  //
  // scopeQ - given to the query (a database that filters on it does), and checked on every entity it returns
  //
  if (scopeQP != NULL)
  {
    if ((scopeQP->type != CorString) || (scopeQP->value.s[0] == 0))
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'scopeQ' is a non-empty string: a scope query");
      return NULL;
    }

    if ((filter.scopeExpr = ldScopeExprParse(scopeQP->value.s, &corRest.kalloc)) == NULL)
      return NULL;                                    // ldScopeExprParse has raised the 400
  }

  CorNode* entitiesP = NULL;

  if ((db.entityQuery == NULL) || (db.entityQuery((Tenant*) corNgsild.tenantP, &filter, &entitiesP) != DB_OK) || (entitiesP == NULL))
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error selecting the entities");
    return NULL;
  }

  CorNode* execP = parentBuild(bP, "GroupedServiceExecution", str(bodyP, "id"), methodP->value.s, NULL);

  if (execP == NULL)
    return NULL;

  //
  // The selection, as given (the report's Retrieve shows it)
  //
  const char* kept[] = { "serviceName", "entityType", "idPattern", "q", "geoQ", "scopeQ", "executionInput", NULL };

  for (int ix = 0; kept[ix] != NULL; ix++)
  {
    CorNode* mP = corTreeLookup(bodyP, kept[ix]);

    if (mP != NULL)
      corTreeChildAdd(execP, corTreeClone(corRest.kallocP, mP));
  }

  for (CorNode* eP = entitiesP->value.head; eP != NULL; eP = eP->next)
  {
    const char* entityId = str(eP, "id");
    int         status   = 0;
    char        why[512];

    if (entityId == NULL)
      continue;

    if ((filter.scopeExpr != NULL) && (ldEntityMatchScope(corTreeLookup(eP, "scope"), filter.scopeExpr) == false))
      continue;                                       // out of the scope: not one of the group

    CorNode* childP = seExecutionBuild(entityId, serviceName, inputP, NULL, &status, why, sizeof(why));

    if ((childP == NULL) && (status == 404))
      continue;                                       // it does not offer the service: not one of the group
    else if (childP == NULL)
      return buildRefused(status, why);

    if (bP->docN >= 255)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "too many service executions in one request (255 at most)");
      return NULL;
    }

    corTreeChildAdd(childP, corTreeString(corRest.kallocP, "_parent", str(execP, "id")));
    bP->docV[bP->docN++] = childP;
    childAdd(execP, childP);
  }

  if (corTreeLookup(execP, "_children")->value.head == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "no entity of the selection offers the service '%s'", corLdCompact(corNgsild.contextP, serviceName));
    return NULL;
  }

  return execP;
}



// -----------------------------------------------------------------------------
//
// startNode - start an execution: a simple one handed to its executor; a parent, its first child (sequential)
// or all of them (parallel)
//
static void startNode(const char* execId)
{
  CorNode* execP = NULL;

  if (db.docRetrieve((Tenant*) corNgsild.tenantP, "serviceExecutions", execId, &execP) != DB_OK)
    return;

  const char* type = str(execP, "type");

  if (strcmp(type, "ServiceExecution") == 0)
  {
    SeForward result;

    seExecutionStart(execP, &result);                 // its end (sync, or a failed hand-off) tells its parent
    return;
  }

  char    buf[64];
  int64_t now = nowNs();

  member(execP, corTreeString(corRest.kallocP, "executionStatus", "executing"));
  ldSysTimestampToIso((long long) now, buf, sizeof(buf));
  member(execP, corTreeString(corRest.kallocP, "executionStartedAt", buf));
  seExecutionStore((Tenant*) corNgsild.tenantP, execP);

  CorNode* childrenP = corTreeLookup(execP, "_children");
  bool     parallel  = (strcmp(str(execP, "combinationMethod"), "parallel") == 0);

  //
  // The ids first: a synchronous child ends while it is started, and that may end this execution
  //
  char* idV[256];
  int   idN = 0;

  for (CorNode* cP = childrenP->value.head; (cP != NULL) && (idN < 256); cP = cP->next)
    idV[idN++] = cP->value.s;

  for (int ix = 0; ix < idN; ix++)
  {
    startNode(idV[ix]);

    if (parallel == false)
      break;                                          // sequential: the next when this one completed
  }
}



// -----------------------------------------------------------------------------
//
// seCombinedChildEnded -
//
void seCombinedChildEnded(const char* parentId)
{
  Tenant*  tenantP = (Tenant*) corNgsild.tenantP;
  CorNode* parentP = NULL;

  if (db.docRetrieve(tenantP, "serviceExecutions", parentId, &parentP) != DB_OK)
    return;

  if (terminal(str(parentP, "executionStatus")))
    return;

  bool     parallel  = (strcmp(str(parentP, "combinationMethod"), "parallel") == 0);
  CorNode* childrenP = corTreeLookup(parentP, "_children");
  int      done      = 0;
  int      failed    = 0;
  int      cancelled = 0;
  int      total     = 0;
  char*    nextId    = NULL;

  for (CorNode* cP = childrenP->value.head; cP != NULL; cP = cP->next)
  {
    CorNode* childP = NULL;

    total++;

    if (db.docRetrieve(tenantP, "serviceExecutions", cP->value.s, &childP) != DB_OK)
      continue;

    const char* status = str(childP, "executionStatus");

    if      (strcmp(status, "completed") == 0) done++;
    else if (strcmp(status, "failed")    == 0) failed++;
    else if (strcmp(status, "cancelled") == 0) cancelled++;
    else if ((nextId == NULL) && (strcmp(status, "pending") == 0))
      nextId = cP->value.s;
  }

  if (parallel == true)
  {
    if (done + failed + cancelled < total)
      return;                                         // not all of them yet
  }
  else if ((failed == 0) && (cancelled == 0) && (nextId != NULL))
  {
    startNode(nextId);                                // sequential: the next one
    return;
  }
  else if ((failed == 0) && (cancelled == 0) && (done < total))
    return;                                           // one is running

  //
  // Sequential and something went wrong: the children not started never will be
  //
  if ((parallel == false) && ((failed > 0) || (cancelled > 0)))
  {
    for (CorNode* cP = childrenP->value.head; cP != NULL; cP = cP->next)
    {
      CorNode* childP = NULL;

      if ((db.docRetrieve(tenantP, "serviceExecutions", cP->value.s, &childP) == DB_OK) && (strcmp(str(childP, "executionStatus"), "pending") == 0))
      {
        ended(childP, "cancelled");
        corTreeChildRemove(childP, corTreeLookup(childP, "_parent"));   // not reported back: this is the parent ending
        db.docReplace(tenantP, "serviceExecutions", cP->value.s, childP);
      }
    }
  }

  ended(parentP, ((failed == 0) && (cancelled == 0)) ? "completed" : "failed");
  seExecutionStore(tenantP, parentP);
}



// -----------------------------------------------------------------------------
//
// seCombinedCreate -
//
bool seCombinedCreate(CorNode* bodyP)
{
  Tenant*     tenantP = (Tenant*) corNgsild.tenantP;
  const char* type    = str(bodyP, "type");
  Built       built;
  CorNode*    execP   = NULL;

  memset(&built, 0, sizeof(built));

  for (CorNode* mP = bodyP->value.head; mP != NULL; mP = mP->next)
  {
    if (strcmp(mP->name, "executionStatus") == 0)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'executionStatus' is the broker's to set");
      return true;
    }
  }

  if (strcmp(type, "GroupedServiceExecution") == 0)
    execP = buildGrouped(&built, bodyP);
  else if (corTreeLookup(bodyP, "serviceTemplateId") != NULL)
  {
    const char* templateId = str(bodyP, "serviceTemplateId");
    CorNode*    templateP  = NULL;

    if (membersCheck(bodyP, fromTemplateV, "a Combined Service Execution of a template") == false)
      return true;

    if ((templateId == NULL) || (db.docRetrieve(tenantP, "serviceTemplates", templateId, &templateP) != DB_OK))
    {
      ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "Combined Service Template '%s' not found", (templateId != NULL) ? templateId : "");
      return true;
    }

    CorNode*    methodP = corTreeLookup(bodyP, "combinationMethod");
    const char* method  = (methodP != NULL) ? str(bodyP, "combinationMethod") : str(templateP, "combinationMethod");

    if ((methodP != NULL) && (methodOk(methodP) == false))
      return true;

    execP = buildFromTemplate(&built, templateP, corTreeLookup(bodyP, "services"), method, str(bodyP, "id"), NULL);

    if (execP != NULL)
      corTreeChildAdd(execP, corTreeString(corRest.kallocP, "serviceTemplateId", templateId));
  }
  else
    execP = buildCombined(&built, bodyP, NULL);

  if (execP == NULL)
    return true;                                      // refused - the error is raised

  CorNode* notificationP = corTreeLookup(bodyP, "notification");

  if (notificationP != NULL)
    corTreeChildAdd(execP, corTreeClone(corRest.kallocP, notificationP));

  //
  // Stored, all of them - then started
  //
  for (int ix = 0; ix < built.docN; ix++)
  {
    int r = db.docCreate(tenantP, "serviceExecutions", str(built.docV[ix], "id"), built.docV[ix]);

    if (r == DB_ALREADY_EXISTS)
    {
      ldError(409, LD_ERROR_ALREADY_EXISTS, "Already Exists", "Service Execution '%s' already exists", str(built.docV[ix], "id"));
      return true;
    }
    else if (r != DB_OK)
    {
      ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error creating the Service Executions");
      return true;
    }
  }

  const char* execId = str(execP, "id");

  startNode(execId);

  int   size     = 64 + strlen(execId);
  char* location = (char*) corAlloc(&corRest.kalloc, size);

  snprintf(location, size, "/ngsi-ld/v1/services/%s", execId);
  corRestOutHeaderAdd("Location", location);
  corRestOutHeaderAdd("Service-Execution", execId);
  corRest.out.httpStatusCode = 201;

  return true;
}



// -----------------------------------------------------------------------------
//
// cancelTree - cancel a parent's children (recursively); false: one could not be (409)
//
static bool cancelTree(CorNode* execP)
{
  Tenant*  tenantP   = (Tenant*) corNgsild.tenantP;
  CorNode* childrenP = corTreeLookup(execP, "_children");
  bool     all       = true;

  for (CorNode* cP = (childrenP != NULL) ? childrenP->value.head : NULL; cP != NULL; cP = cP->next)
  {
    CorNode* childP = NULL;

    if (db.docRetrieve(tenantP, "serviceExecutions", cP->value.s, &childP) != DB_OK)
      continue;

    if (terminal(str(childP, "executionStatus")))
      continue;

    corTreeChildRemove(childP, corTreeLookup(childP, "_parent"));   // the parent is ending: not told back

    if (strcmp(str(childP, "type"), "ServiceExecution") != 0)
    {
      if (cancelTree(childP) == false)
        all = false;
      else
      {
        ended(childP, "cancelled");
        seExecutionStore(tenantP, childP);
      }
    }
    else
    {
      SeForward result;

      if (seExecutionCancelOne(childP, &result) != 204)
        all = false;
    }
  }

  return all;
}



// -----------------------------------------------------------------------------
//
// deleteTree - an ended parent and its children deleted
//
static void deleteTree(CorNode* execP)
{
  Tenant*  tenantP   = (Tenant*) corNgsild.tenantP;
  CorNode* childrenP = corTreeLookup(execP, "_children");

  for (CorNode* cP = (childrenP != NULL) ? childrenP->value.head : NULL; cP != NULL; cP = cP->next)
  {
    CorNode* childP = NULL;

    if (db.docRetrieve(tenantP, "serviceExecutions", cP->value.s, &childP) == DB_OK)
      deleteTree(childP);
  }

  db.docDelete(tenantP, "serviceExecutions", str(execP, "id"));
}



// -----------------------------------------------------------------------------
//
// seCombinedCancel -
//
bool seCombinedCancel(CorNode* execP)
{
  if (terminal(str(execP, "executionStatus")))
  {
    deleteTree(execP);
    corRest.out.httpStatusCode = 204;
    return true;
  }

  if (cancelTree(execP) == false)
  {
    ldError(409, LD_ERROR_CONFLICT, "Conflict", "an executor cannot cancel a service execution of '%s' - it goes on", str(execP, "id"));
    return true;
  }

  ended(execP, "cancelled");
  seExecutionStore((Tenant*) corNgsild.tenantP, execP);
  corRest.out.httpStatusCode = 204;
  return true;
}



// -----------------------------------------------------------------------------
//
// seCombinedChildrenEmbed -
//
void seCombinedChildrenEmbed(CorNode* execP)
{
  CorNode* childrenP = corTreeLookup(execP, "_children");

  if (childrenP == NULL)
    return;

  CorNode* listP = corTreeArray(corRest.kallocP, "serviceExecutions");

  for (CorNode* cP = childrenP->value.head; cP != NULL; cP = cP->next)
  {
    CorNode* childP = NULL;

    if (db.docRetrieve((Tenant*) corNgsild.tenantP, "serviceExecutions", cP->value.s, &childP) != DB_OK)
      continue;

    seExecutionRenderAll(childP);
    childP->name = NULL;
    corTreeChildAdd(listP, childP);
  }

  corTreeChildAdd(execP, listP);
}



// -----------------------------------------------------------------------------
//
// seTemplateCheck -
//
static bool templateServicesCheck(CorNode* servicesP);

static bool templateCheck(CorNode* templateP, bool top)
{
  CorNode* methodP = corTreeLookup(templateP, "combinationMethod");

  if (membersCheck(templateP, (top == true) ? templateMemberV : nestedTmplV, (top == true) ? "a Combined Service Template" : "a nested Combined Service Template") == false)
    return false;

  if ((top == true) && (str(templateP, "templateName") == NULL))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "a Combined Service Template needs a 'templateName'");
    return false;
  }

  if (methodOk(methodP) == false)
    return false;

  return templateServicesCheck(corTreeLookup(templateP, "services"));
}

static bool templateServicesCheck(CorNode* servicesP)
{
  if ((servicesP == NULL) || (servicesP->type != CorArray) || (servicesP->value.head == NULL))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "a Combined Service Template needs 'services': a non-empty array");
    return false;
  }

  for (CorNode* sP = servicesP->value.head; sP != NULL; sP = sP->next)
  {
    if (sP->type != CorObject)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "a member of 'services' is a service or a CombinedServiceTemplate");
      return false;
    }

    if ((str(sP, "type") != NULL) && (strcmp(str(sP, "type"), "CombinedServiceTemplate") == 0))
    {
      if (templateCheck(sP, false) == false)
        return false;
      continue;
    }

    if (membersCheck(sP, tmplServiceV, "a service of a Combined Service Template") == false)
      return false;

    if ((str(sP, "serviceName") == NULL) || (str(sP, "entityType") == NULL))
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "a service of a template names its 'serviceName' and its 'entityType'");
      return false;
    }

    CorNode* inputP = corTreeLookup(sP, "executionInput");

    if ((inputP != NULL) && (inputP->type != CorObject))
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'executionInput' is a JSON object");
      return false;
    }
  }

  return true;
}

bool seTemplateCheck(CorNode* templateP)
{
  if ((templateP == NULL) || (templateP->type != CorObject) || (str(templateP, "type") == NULL) || (strcmp(str(templateP, "type"), "CombinedServiceTemplate") != 0))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "a Combined Service Template's 'type' is \"CombinedServiceTemplate\"");
    return false;
  }

  return templateCheck(templateP, true);
}

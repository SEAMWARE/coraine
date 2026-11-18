//
// FILE            seExecution.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // qsort
#include <pthread.h>                                  // pthread_mutex_*
#include <string.h>                                   // strcmp, strlen, strncmp
#include <strings.h>                                  // strcasecmp
#include <time.h>                                     // clock_gettime

#include "corBase/corUriPasswordMask.h"               // corUriPasswordMask
#include "corLog/corLog.h"                            // COR_T, COR_W
#include "corAlloc/corAlloc.h"                        // corAlloc, corAllocStrdup
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeString, corTreeInteger, ...
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corJson/corJsonRender.h"                    // corJsonFastRender
#include "corJson/corJsonRenderSize.h"                // corJsonFastRenderSize
#include "corJsonld/corLdCompact.h"                   // corLdCompact
#include "corJsonld/corLdExpand.h"                    // corLdExpand
#include "corTree/corTreeChildReplace.h"              // corTreeChildReplace
#include "corJsonld/corLdCompactTree.h"               // corLdCompactTreeWith
#include "corJsonld/corLdDownload.h"                  // corLdContextFromUrl
#include "corRest/CorRestState.h"                     // corRest
#include "corRest/corRestOutHeader.h"                 // corRestOutHeaderAdd
#include "corRest/corRestClient.h"                    // corRestClient*
#include "corNgsild/CorNgsild.h"                      // corNgsild
#include "corNgsild/ldError.h"                        // ldError
#include "corNgsild/LdProblem.h"                      // LD_ERROR_*
#include "corNgsild/ldIdGenerate.h"                   // ldIdGenerate
#include "corNgsild/ldSysTimestamp.h"                 // ldSysTimestampCreate, ldSysTimestampModify, ldSysTimestampToIso, ...
#include "corNgsild/ldStripSysAttrs.h"                // ldStripSysAttrs
#include "corNgsild/ldTenantHeader.h"                 // ldTenantHeaderAdd
#include "corNgsild/ldNotifyTransport.h"              // ldNotifyIsHttp, ldNotifyTransportSend
#include "corNgsild/ldIso8601Duration.h"              // ldIso8601DurationParseNs
#include "corNgsild/ldServiceDescription.h"           // ldServiceDescriptionIs
#include "corNgsild/LdForwarding.h"                   // LdForwardRequest, LdForwardResponse, LdForwardingPlugin
#include "corNgsild/ldForwarding.h"                   // ldForwardingForEndpoint

#include "db/DbDriver.h"                              // db, DB_*
#include "db/Tenant.h"                                // Tenant
#include "bridge/bridgeService.h"                     // bridgeServiceExecute, bridgeServiceCancel
#include "corBridge/BridgeBroker.h"                   // BRIDGE_*
#include "serviceExecution/seJsonSchema.h"            // seJsonSchemaCheck
#include "serviceExecution/seRegistrationMatch.h"     // seRegistrationMatches
#include "serviceExecution/seRetention.h"            // seRetentionPush, seRetentionSeeded, seRetentionMaxEnded
#include "serviceExecution/seCombined.h"              // seCombinedChildEnded, seCombinedCancel, seCombinedChildrenEmbed
#include "bridge/bridgeSampleIn.h"                    // bridgeThreadBind
#include "serviceExecution/seExecution.h"             // Own interface
#include "coraineTraceLevels.h"                       // CtService



// -----------------------------------------------------------------------------
//
// Defaults - the time a service may take, and how long a finished execution is kept
//
volatile bool seRemoteExecutors = false;              // a service is executed over http(s):// or cor:// (seRemoteExecutorsSet)
int64_t seExecutionRetentionNs = 3600LL * 1000000000LL;       // --executionRetention (seExecutionRetentionSet)
static int64_t retentionFromNs  = 0;                           // the broker's start: what ended before is seeded



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
// member - set (replace or add) a member of an object
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
// str / num - a member's string / integer value, or the default
//
static const char* str(CorNode* objectP, const char* name)
{
  CorNode* nodeP = corTreeLookup(objectP, name);

  return ((nodeP != NULL) && (nodeP->type == CorString)) ? nodeP->value.s : NULL;
}

static int64_t num(CorNode* objectP, const char* name)
{
  CorNode* nodeP = corTreeLookup(objectP, name);

  return ((nodeP != NULL) && (nodeP->type == CorInt)) ? (int64_t) nodeP->value.i : 0;
}



// -----------------------------------------------------------------------------
//
// timestamp - an ISO 8601 member (executionStartedAt / executionEndedAt) and its '_..Ns' twin
//
static void timestamp(CorNode* execP, const char* isoName, const char* nsName, int64_t ns)
{
  char buf[64];

  ldSysTimestampToIso((long long) ns, buf, sizeof(buf));
  member(execP, corTreeString(corRest.kallocP, isoName, buf));
  member(execP, corTreeInteger(corRest.kallocP, nsName, ns));
}



// -----------------------------------------------------------------------------
//
// terminal - is the status final?
//
static bool terminal(const char* status)
{
  return (status != NULL) && ((strcmp(status, "completed") == 0) || (strcmp(status, "failed") == 0) || (strcmp(status, "cancelled") == 0));
}



// -----------------------------------------------------------------------------
//
// problem - a ProblemDetails tree
//
static CorNode* problem(const char* type, const char* title, int status, const char* detail)
{
  CorNode* pP = corTreeObject(corRest.kallocP, "executionError");

  corTreeChildAdd(pP, corTreeString(corRest.kallocP, "type", type));
  corTreeChildAdd(pP, corTreeString(corRest.kallocP, "title", title));
  corTreeChildAdd(pP, corTreeInteger(corRest.kallocP, "status", status));
  corTreeChildAdd(pP, corTreeString(corRest.kallocP, "detail", detail));

  return pP;
}



// -----------------------------------------------------------------------------
//
// seExecutionRender -
//
void seExecutionRender(CorNode* execP)
{
  CorNode* mP = execP->value.head;

  while (mP != NULL)
  {
    CorNode* nextP = mP->next;

    if ((mP->name != NULL) && (mP->name[0] == '_'))
      corTreeChildRemove(execP, mP);

    mP = nextP;
  }

  if (corNgsild.sysAttrs == false)
    ldStripSysAttrs(execP);
  else
    ldSysTimestampsToIso(execP, &corRest.kalloc);
}



// -----------------------------------------------------------------------------
//
// seExecutionRenderAll - seExecutionRender, and a combined or grouped execution's children embedded
//
void seExecutionRenderAll(CorNode* execP)
{
  seCombinedChildrenEmbed(execP);
  seExecutionRender(execP);
}



// -----------------------------------------------------------------------------
//
// notify - the execution's change, to its notification endpoint
//
// An NGSI-LD Notification, data = [ the execution ], compacted with the @context of the request that
// created the execution. http(s) is POSTed; any other URI goes through the transports (WebSocket,
// mqtt://) as a Subscription's notification does. A failure is logged, and the execution goes on.
//
static void report(CorNode* execP);

static void notify(CorNode* execP)
{
  if (corTreeLookup(execP, "_reportTo") != NULL)
    report(execP);                                    // we are another broker's executor: it gets our reports

  CorNode*    notificationP = corTreeLookup(execP, "notification");
  CorNode*    endpointP     = (notificationP != NULL) ? corTreeLookup(notificationP, "endpoint") : NULL;
  const char* uri           = (endpointP != NULL) ? str(endpointP, "uri") : NULL;

  if (uri == NULL)
    return;

  CorNode* renderedP = corTreeClone(corRest.kallocP, execP);
  bool     sysAttrs  = corNgsild.sysAttrs;

  corNgsild.sysAttrs = false;
  seExecutionRenderAll(renderedP);
  corNgsild.sysAttrs = sysAttrs;

  const char*   contextUrl = str(execP, "_context");
  CorLdContext* contextP   = (contextUrl != NULL) ? corLdContextFromUrl(contextUrl, &corRest.kalloc) : NULL;

  corLdCompactTreeWith(renderedP, contextP, &corRest.kalloc);

  char     at[64];
  CorNode* bodyP = corTreeObject(corRest.kallocP, NULL);
  CorNode* dataP = corTreeArray(corRest.kallocP, "data");

  ldSysTimestampToIso((long long) nowNs(), at, sizeof(at));
  corTreeChildAdd(bodyP, corTreeString(corRest.kallocP, "id", ldIdGenerate(&corRest.kalloc, "Notification")));
  corTreeChildAdd(bodyP, corTreeString(corRest.kallocP, "type", "Notification"));
  corTreeChildAdd(bodyP, corTreeString(corRest.kallocP, "notifiedAt", at));
  renderedP->name = NULL;
  corTreeChildAdd(dataP, renderedP);
  corTreeChildAdd(bodyP, dataP);

  int   size = corJsonFastRenderSize(bodyP) + 1;
  char* body = (char*) corAlloc(&corRest.kalloc, size);
  char  link[600] = "";

  corJsonFastRender(bodyP, body);

  if (contextUrl != NULL)
    snprintf(link, sizeof(link), "<%s>; rel=\"http://www.w3.org/ns/json-ld#context\"; type=\"application/ld+json\"", contextUrl);

  //
  // cor:// - a client (another broker, say) listening on cor://: the notification is a POST over the
  // cor:// forwarding transport
  //
  if (strncmp(uri, "cor://", 6) == 0)
  {
    const LdForwardingPlugin* pluginP = ldForwardingForEndpoint(uri);
    LdForwardRequest          freq;
    LdForwardResponse         fresp;
    CorRestKeyValue           headerV[3];
    int                       headers = 0;

    headerV[headers].key = (char*) "Content-Type"; headerV[headers].value = (char*) "application/json"; headerV[headers++].bit = 0;
    if (link[0] != 0)
    {
      headerV[headers].key = (char*) "Link"; headerV[headers].value = link; headerV[headers++].bit = 0;
    }
    if ((corNgsild.tenantName != NULL) && (corNgsild.tenantName[0] != 0))
    {
      headerV[headers].key = (char*) "NGSILD-Tenant"; headerV[headers].value = (char*) corNgsild.tenantName; headerV[headers++].bit = 0;
    }

    memset(&freq, 0, sizeof(freq));
    memset(&fresp, 0, sizeof(fresp));
    freq.endpoint         = uri;
    freq.verb             = CorVerbPost;
    freq.headerV          = headerV;
    freq.headerCount      = headers;
    freq.body             = body;
    freq.bodyLen          = (int) strlen(body);
    freq.connectTimeoutMs = 5000;
    freq.requestTimeoutMs = 10000;
    fresp.allocP          = &corRest.kalloc;

    if ((pluginP == NULL) || (pluginP->send == NULL) || (pluginP->send(&freq, &fresp) != 0) || (fresp.statusCode < 200) || (fresp.statusCode > 299))
    {
      char shown[1024];
      COR_W("Service Execution '%s': notification to %s FAILED (%d)", str(execP, "id"), corUriPasswordMask(uri, shown, sizeof(shown)), fresp.statusCode);
    }
    return;
  }

  if (ldNotifyIsHttp(uri) == false)
  {
    if (ldNotifyTransportSend(uri, body, "application/json", (link[0] != 0) ? link : NULL, NULL, NULL) == false)
    {
      char shown[1024];
      COR_W("Service Execution '%s': notification to %s FAILED", str(execP, "id"), corUriPasswordMask(uri, shown, sizeof(shown)));
    }
    return;
  }

  CorRestClientRequest  req;
  CorRestClientResponse resp;

  corRestClientRequestInit(&req, CorVerbPost, uri, NULL);
  corRestClientRequestHeader(&req, "Content-Type", "application/json");
  if (link[0] != 0)
    corRestClientRequestHeader(&req, "Link", link);
  ldTenantHeaderAdd(&req, corNgsild.tenantName);
  corRestClientRequestBody(&req, body, strlen(body));
  corRestClientRequestTimeout(&req, 5000, 10000);
  corRestClientSend(&req, &resp);

  if ((resp.statusCode < 200) || (resp.statusCode > 299))
  {
    char shown[1024];
    COR_W("Service Execution '%s': notification to %s FAILED (%d)", str(execP, "id"), corUriPasswordMask(uri, shown, sizeof(shown)), resp.statusCode);
  }

  corRestClientResponseCleanup(&resp);
}



// -----------------------------------------------------------------------------
//
// storeMutex - every store re-reads the record under it: the writers (a request, a bridge's report on
// the bridge's thread, the sweep) each read the record before they change it, and an ended execution
// replaced by a copy read before it ended would be running again
//
static pthread_mutex_t storeMutex = PTHREAD_MUTEX_INITIALIZER;



// -----------------------------------------------------------------------------
//
// evict - an ended top-level execution deleted with its tree (gone meanwhile: nothing)
//
static void evict(Tenant* tenantP, const char* execId)
{
  CorNode* execP = NULL;

  if (db.docRetrieve(tenantP, "serviceExecutions", execId, &execP) == DB_OK)
  {
    COR_T(CtService, "Service Execution '%s': one ended execution too many - deleted", execId);
    seCombinedDeleteTree(tenantP, execP);
  }
}



// -----------------------------------------------------------------------------
//
// retain - an ended top-level execution counted; the oldest past the cap goes
//
static void retain(Tenant* tenantP, const char* execId)
{
  char* evictedId = seRetentionPush(tenantP, execId);

  if (evictedId != NULL)
  {
    evict(tenantP, evictedId);
    free(evictedId);
  }
}



// -----------------------------------------------------------------------------
//
// store - the execution replaced in the database, and notified
//
// One that ended meanwhile (cancelled, completed, timed out) changes no more: nothing is stored, and
// that is no error.
//
bool seExecutionStore(Tenant* tenantP, CorNode* execP)
{
  const char* execId    = str(execP, "id");
  CorNode*    currentP  = NULL;
  bool        ended     = false;
  bool        stored    = false;

  ldSysTimestampModify(execP);

  pthread_mutex_lock(&storeMutex);

  if ((db.docRetrieve(tenantP, "serviceExecutions", execId, &currentP) == DB_OK) && terminal(str(currentP, "executionStatus")))
    ended = true;
  else
    stored = (db.docReplace(tenantP, "serviceExecutions", execId, execP) == DB_OK);

  pthread_mutex_unlock(&storeMutex);

  if (ended == true)
  {
    COR_T(CtService, "Service Execution '%s' ended meanwhile - not stored", execId);
    return true;
  }

  if (stored == false)
  {
    COR_W("Service Execution '%s': could not be stored", execId);
    return false;
  }

  notify(execP);

  //
  // A child of a combined or grouped execution that ended: its parent goes on (the next one), or ends
  //
  const char* parentId = str(execP, "_parent");

  if ((parentId != NULL) && terminal(str(execP, "executionStatus")))
  {
    if (corTreeLookup(execP, "_parentEnding") == NULL)  // a parent that is ending itself is not told
      seCombinedChildEnded(parentId);
  }
  else if ((parentId == NULL) && terminal(str(execP, "executionStatus")))
    retain(tenantP, execId);                          // a top-level one ended: the cap (--executionMaxEnded)

  return true;
}



// -----------------------------------------------------------------------------
//
// finish - the execution's terminal status
//
static void finish(CorNode* execP, const char* status, CorNode* outputP, CorNode* errorP)
{
  member(execP, corTreeString(corRest.kallocP, "executionStatus", status));
  timestamp(execP, "executionEndedAt", "_endedNs", nowNs());

  if (outputP != NULL)
  {
    outputP->name = (char*) "executionOutput";
    member(execP, outputP);
  }

  if (errorP != NULL)
  {
    errorP->name = (char*) "executionError";
    member(execP, errorP);
  }
}



// -----------------------------------------------------------------------------
//
// idCompare - qsort of registration ids
//
static int idCompare(const void* a, const void* b)
{
  return strcmp(*(const char* const*) a, *(const char* const*) b);
}



// -----------------------------------------------------------------------------
//
// serviceFind - the Service Registration offering 'serviceName' for the entity; NULL: none (*statusP 404),
// or more than one (*statusP 409, why naming them)
//
// A service name is unique within an entity - a registration that would break that is refused (409), but
// registrations made before that rule, or an entity of two types each with a registration of the name, can
// still offer one name twice. Which one would run is then not the broker's to guess.
//
static CorNode* serviceFind(Tenant* tenantP, const char* entityId, CorNode* entityP, const char* serviceName, int* statusP, char* why, int whySize)
{
  CorNode* regsP = NULL;

  //
  // The entity's own description of the service first (GR CIM-055 § 6.3.3) - an attribute named by the
  // service, of type ServiceDescription: made into the registration it stands for
  //
  CorNode* attrP = corTreeLookup(entityP, serviceName);
  CorNode* instP = (attrP != NULL) ? corTreeLookup(attrP, "@none") : NULL;

  if (ldServiceDescriptionIs(instP))
  {
    CorNode*    regP     = corTreeObject(corRest.kallocP, NULL);
    CorNode*    siP      = corTreeObject(corRest.kallocP, "serviceInformation");
    const char* copied[] = { "mode", "inputSchema", "outputSchema", NULL };

    corTreeChildAdd(regP, corTreeString(corRest.kallocP, "id", entityId));
    corTreeChildAdd(regP, corTreeString(corRest.kallocP, "endpoint", str(instP, "endpoint")));
    corTreeChildAdd(siP,  corTreeString(corRest.kallocP, "serviceName", serviceName));

    for (int ix = 0; copied[ix] != NULL; ix++)
    {
      CorNode* mP = corTreeLookup(instP, copied[ix]);

      if (mP != NULL)
        corTreeChildAdd(siP, corTreeClone(corRest.kallocP, mP));
    }

    corTreeChildAdd(regP, siP);
    return regP;
  }

  if ((db.docQuery == NULL) || (db.docQuery(tenantP, "serviceRegistrations", &regsP) != DB_OK))
    return NULL;

  //
  // The entity's types, expanded as stored: "type" a string or an array
  //
  CorNode* typeP = corTreeLookup(entityP, "type");
  char*    typeV[16];
  int      typeN = 0;

  if ((typeP != NULL) && (typeP->type == CorString))
    typeV[typeN++] = typeP->value.s;
  else if ((typeP != NULL) && (typeP->type == CorArray))
  {
    for (CorNode* tP = typeP->value.head; (tP != NULL) && (typeN < 15); tP = tP->next)
    {
      if (tP->type == CorString)
        typeV[typeN++] = tP->value.s;
    }
  }
  typeV[typeN] = NULL;

  CorNode*    foundP = NULL;
  const char* idV[8];
  int         found  = 0;

  for (CorNode* regP = regsP->value.head; regP != NULL; regP = regP->next)
  {
    CorNode*    siP  = corTreeLookup(regP, "serviceInformation");
    const char* name = (siP != NULL) ? str(siP, "serviceName") : NULL;

    if ((name == NULL) || (strcmp(name, serviceName) != 0) || (seRegistrationMatches(regP, entityId, typeV, entityP) == false))
      continue;

    if (found == 0)
      foundP = regP;

    if (found < 8)
      idV[found] = (str(regP, "id") != NULL) ? str(regP, "id") : "";

    found++;
  }

  if (found > 1)
  {
    int  idN    = (found < 8) ? found : 8;
    char ids[512];
    int  idsLen = 0;

    qsort(idV, idN, sizeof(idV[0]), idCompare);       // named in one order whatever the database's

    ids[0] = 0;
    for (int ix = 0; (ix < idN) && (idsLen < (int) sizeof(ids) - 1); ix++)
      idsLen += snprintf(&ids[idsLen], sizeof(ids) - idsLen, "%s'%s'", (ix == 0) ? "" : ", ", idV[ix]);

    *statusP = 409;
    snprintf(why, whySize, "the service '%s' of entity '%s' is offered by %d Service Registrations: %s - a service name is unique within an entity",
             corLdCompact(corNgsild.contextP, serviceName, &corRest.kalloc), entityId, found, ids);
    return NULL;
  }

  return foundP;
}



// -----------------------------------------------------------------------------
//
// responseBody - the executor's body: JSON, or the text as a JSON string; NULL for none
//
static CorNode* responseBody(CorRestClientResponse* respP)
{
  if ((respP->body == NULL) || (respP->bodyLen == 0))
    return NULL;

  char* copy = (char*) corAlloc(&corRest.kalloc, respP->bodyLen + 1);

  memcpy(copy, respP->body, respP->bodyLen);
  copy[respP->bodyLen] = 0;

  CorNode* treeP = corJsonParse(corRest.corJsonP, copy);

  if (treeP != NULL)
    return treeP;

  memcpy(copy, respP->body, respP->bodyLen);          // the parser may have written into it
  copy[respP->bodyLen] = 0;

  return corTreeString(corRest.kallocP, NULL, copy);
}



// -----------------------------------------------------------------------------
//
// expandedLookup - a member by its name expanded with the request's @context, else as written
//
static CorNode* expandedLookup(CorNode* containerP, const char* name)
{
  if ((containerP == NULL) || (containerP->type != CorObject))
    return NULL;

  CorNode* nodeP = corTreeLookup(containerP, corLdExpand(corNgsild.contextP, name, &corRest.kalloc, NULL, NULL));

  return (nodeP != NULL) ? nodeP : corTreeLookup(containerP, name);
}



// -----------------------------------------------------------------------------
//
// selectorValue - an AttributeSelector's value: an attribute of a stored entity, by a q-language path
//
//   attr            the attribute's value
//   attr.sub        a sub-attribute's value
//   attr[key][...]  a member of a JSON value
//
// A stored attribute is its instances by datasetId - the default one ("@none") is the selected one.
// NULL when the path leads nowhere.
//
static CorNode* selectorValue(CorNode* entityP, const char* path)
{
  char* copy = corAllocStrdup(&corRest.kalloc, path);
  char* keys = strchr(copy, '[');

  if (keys != NULL)
    *keys = 0;

  //
  // attr.sub.sub ...
  //
  char*    save  = NULL;
  char*    part  = strtok_r(copy, ".", &save);
  CorNode* attrP = expandedLookup(entityP, part);

  if (attrP == NULL)
    return NULL;

  CorNode* instP = corTreeLookup(attrP, "@none");

  if (instP == NULL)
    instP = attrP;

  while ((part = strtok_r(NULL, ".", &save)) != NULL)
  {
    if ((instP = expandedLookup(instP, part)) == NULL)
      return NULL;
  }

  CorNode* valueP = corTreeLookup(instP, "value");

  if (valueP == NULL)
    valueP = corTreeLookup(instP, "object");

  //
  // [key][key] ...
  //
  while ((valueP != NULL) && (keys != NULL))
  {
    char* key = keys + 1;
    char* end = strchr(key, ']');

    if (end == NULL)
      return NULL;

    *end   = 0;
    valueP = expandedLookup(valueP, key);
    keys   = (end[1] == '[') ? &end[1] : NULL;
  }

  return valueP;
}



// -----------------------------------------------------------------------------
//
// inputResolve - the input with each AttributeSelector replaced by the value it selects (GR CIM-055 § 6.3.5.1)
//
// A member { "type": "AttributeSelector", "attributeSelector": <path>, "entityId"? } takes its value
// from that attribute of that entity (the execution's own when no entityId) - read now, at the hand-off.
// false: one led nowhere, and why.
//
static bool inputResolve(CorNode* execP, CorNode* inP, char* why, int whySize)
{
  Tenant*  tenantP     = (Tenant*) corNgsild.tenantP;
  CorNode* ownEntityP  = NULL;

  for (CorNode* mP = ((inP != NULL) && (inP->type == CorObject)) ? inP->value.head : NULL; mP != NULL; mP = mP->next)
  {
    const char* type = (mP->type == CorObject) ? str(mP, "type") : NULL;

    if ((type == NULL) || (strcmp(type, "AttributeSelector") != 0))
      continue;

    const char* path     = str(mP, "attributeSelector");
    const char* entityId = str(mP, "entityId");
    CorNode*    entityP  = NULL;

    if (path == NULL)
    {
      snprintf(why, whySize, "input '%s': an AttributeSelector needs its 'attributeSelector'", mP->name);
      return false;
    }

    if (entityId == NULL)
    {
      if ((ownEntityP == NULL) && (db.entityRetrieve(tenantP, str(execP, "entityId"), &ownEntityP) != DB_OK))
        ownEntityP = NULL;
      entityP = ownEntityP;
    }
    else if (db.entityRetrieve(tenantP, entityId, &entityP) != DB_OK)
      entityP = NULL;

    CorNode* valueP = (entityP != NULL) ? selectorValue(entityP, path) : NULL;

    if (valueP == NULL)
    {
      snprintf(why, whySize, "input '%s': no attribute '%s' on entity '%s'", mP->name, path, (entityId != NULL) ? entityId : str(execP, "entityId"));
      return false;
    }

    CorNode* cloneP = corTreeClone(corRest.kallocP, valueP);

    cloneP->name = mP->name;
    cloneP->next = mP->next;
    corTreeChildReplace(inP, mP, cloneP);
    mP = cloneP;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// invocationBody - what the executor receives: the entity's id and type (compacted with the request's
// @context), and the input - its AttributeSelectors resolved. NULL: one could not be (why)
//
static char* invocationBody(CorNode* execP, char* why, int whySize)
{
  CorNode*    bodyP = corTreeObject(corRest.kallocP, NULL);
  CorNode*    inP   = corTreeClone(corRest.kallocP, corTreeLookup(execP, "executionInput"));
  const char* type  = str(execP, "entityType");

  if ((inP != NULL) && (inputResolve(execP, inP, why, whySize) == false))
    return NULL;

  corTreeChildAdd(bodyP, corTreeString(corRest.kallocP, "id", str(execP, "entityId")));
  corTreeChildAdd(bodyP, corTreeString(corRest.kallocP, "type", (type != NULL) ? corLdCompact(corNgsild.contextP, type, &corRest.kalloc) : ""));

  for (CorNode* mP = ((inP != NULL) && (inP->type == CorObject)) ? inP->value.head : NULL; mP != NULL; mP = mP->next)
  {
    if ((strcmp(mP->name, "id") != 0) && (strcmp(mP->name, "type") != 0))
      corTreeChildAdd(bodyP, corTreeClone(corRest.kallocP, mP));
  }

  int   size = corJsonFastRenderSize(bodyP) + 1;
  char* body = (char*) corAlloc(&corRest.kalloc, size);

  corJsonFastRender(bodyP, body);
  return body;
}



// -----------------------------------------------------------------------------
//
// jsonTree - a JSON text into a tree in the request's arena; NULL for NULL or unparsable
//
static CorNode* jsonTree(const char* json)
{
  if (json == NULL)
    return NULL;

  char* copy = corAllocStrdup(&corRest.kalloc, json);

  return (copy != NULL) ? corJsonParse(corRest.corJsonP, copy) : NULL;
}



// -----------------------------------------------------------------------------
//
// forwardBridge - the executor is a loaded bridge (its scheme): the broker executes the service
//
static void forwardBridge(CorNode* execP, bool cancel, SeForward* resultP)
{
  const char* endpoint  = str(execP, "_endpoint");
  const char* execId    = str(execP, "id");
  const char* mode      = str(execP, "_mode");
  bool        sync      = (mode != NULL) && (strcmp(mode, "synchronous") == 0);
  int64_t     timeoutNs = num(execP, "_timeoutNs");

  if (cancel == true)
  {
    int r = bridgeServiceCancel(endpoint, execId);

    if ((r == BRIDGE_OK) || (r == BRIDGE_NOT_FOUND))  // cancelled, or nothing running any more
      resultP->accepted = true;
    else if (r == BRIDGE_UNSUPPORTED)
    {
      resultP->clientStatus = 409;
      resultP->errorP       = problem(LD_ERROR_CONFLICT, "Conflict", 409, "the bridge cannot pre-empt it");
    }
    else
    {
      resultP->clientStatus = 502;
      resultP->errorP       = problem(LD_ERROR_INTERNAL_ERROR, "Executor Refused", 502, "the bridge could not cancel it");
    }
    return;
  }

  char  why[512];
  char* body = invocationBody(execP, why, sizeof(why));

  if (body == NULL)
  {
    resultP->clientStatus = 400;
    resultP->errorP       = problem(LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", 400, why);
    return;
  }

  BridgeServiceResult outcome;
  int                 r = bridgeServiceExecute(endpoint, execId, corNgsild.tenantName, body, (sync == true) ? (int) (timeoutNs / 1000000) : 0, &outcome);

  if (r == BRIDGE_NOT_FOUND)
  {
    resultP->clientStatus = 503;
    resultP->errorP       = problem(LD_ERROR_INTERNAL_ERROR, "Executor Unreachable", 503, "no loaded bridge executes this service");
  }
  else if (r == BRIDGE_BAD_INPUT)
  {
    resultP->clientStatus = 400;
    resultP->errorP       = problem(LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", 400, "the service's bridge cannot use this input");
  }
  else if (r != BRIDGE_OK)
  {
    resultP->clientStatus = 502;
    resultP->errorP       = problem(LD_ERROR_INTERNAL_ERROR, "Executor Refused", 502, "the service's bridge refused it");
  }
  else if (sync == false)
    resultP->accepted = true;
  else if (strcmp(outcome.status, "completed") == 0)
  {
    resultP->accepted = true;
    resultP->outputP  = jsonTree(outcome.outputJson);
  }
  else if (strcmp(outcome.status, "failed") == 0)
  {
    CorNode* errorP = jsonTree(outcome.errorJson);

    resultP->clientStatus = 502;
    resultP->errorP       = ((errorP != NULL) && (errorP->type == CorObject)) ? errorP : problem(LD_ERROR_INTERNAL_ERROR, "Execution Failed", 502, "the service's bridge reported a failure");
  }
  else
  {
    resultP->clientStatus = 504;
    resultP->errorP       = problem(LD_ERROR_INTERNAL_ERROR, "Executor Timeout", 504, "the service's bridge did not answer in time");
  }

  bridgeServiceResultRelease(&outcome);
}



// -----------------------------------------------------------------------------
//
// forwardingBody - a cor:// answer as a tree: what the peer sent as a tree, else its JSON text parsed
// (or the text as a JSON string); NULL for none
//
static CorNode* forwardingBody(LdForwardResponse* respP)
{
  if (respP->bodyTree != NULL)
    return respP->bodyTree;

  if ((respP->body == NULL) || (respP->bodyLen <= 0))
    return NULL;

  char* copy = (char*) corAlloc(&corRest.kalloc, respP->bodyLen + 1);

  memcpy(copy, respP->body, respP->bodyLen);
  copy[respP->bodyLen] = 0;

  CorNode* treeP = corJsonParse(corRest.corJsonP, copy);

  if (treeP != NULL)
    return treeP;

  char* text = (char*) corAlloc(&corRest.kalloc, respP->bodyLen + 1);

  memcpy(text, respP->body, respP->bodyLen);
  text[respP->bodyLen] = 0;

  return corTreeString(corRest.kallocP, NULL, text);
}



// -----------------------------------------------------------------------------
//
// SeExchange - the outcome of a request to an executor, or of a report to the broker that asked
//
typedef struct SeExchange
{
  int      status;                                    // the HTTP status (0: no answer)
  int      error;                                     // 0, SE_EXCHANGE_TIMEOUT or SE_EXCHANGE_UNREACHABLE
  char     detail[256];                               // what went wrong in the transport
  CorNode* bodyP;                                     // the answer's body, in the request's arena
} SeExchange;

#define SE_EXCHANGE_TIMEOUT      1
#define SE_EXCHANGE_UNREACHABLE  2



// -----------------------------------------------------------------------------
//
// exchange - one request over http(s):// or cor:// (the scheme says which), with the Service Execution
// headers: Service-Execution, the tenant, the body's Content-Type
//
static void exchange(const char* url, CorRestVerb verb, const char* execId, const char* body, int requestTimeoutMs, SeExchange* xP)
{
  memset(xP, 0, sizeof(SeExchange));

  if (strncmp(url, "cor://", 6) == 0)
  {
    const LdForwardingPlugin* pluginP = ldForwardingForEndpoint(url);
    CorRestKeyValue           headerV[3];
    int                       headers = 0;

    if ((pluginP == NULL) || (pluginP->send == NULL))
    {
      xP->error = SE_EXCHANGE_UNREACHABLE;
      snprintf(xP->detail, sizeof(xP->detail), "no cor:// transport loaded");
      return;
    }

    headerV[headers].key = (char*) "Service-Execution"; headerV[headers].value = (char*) execId; headerV[headers++].bit = 0;
    if (body != NULL)
    {
      headerV[headers].key = (char*) "Content-Type"; headerV[headers].value = (char*) "application/json"; headerV[headers++].bit = 0;
    }
    if ((corNgsild.tenantName != NULL) && (corNgsild.tenantName[0] != 0))
    {
      headerV[headers].key = (char*) "NGSILD-Tenant"; headerV[headers].value = (char*) corNgsild.tenantName; headerV[headers++].bit = 0;
    }

    LdForwardRequest  req;
    LdForwardResponse resp;

    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    req.endpoint         = url;
    req.verb             = verb;
    req.headerV          = headerV;
    req.headerCount      = headers;
    req.body             = body;
    req.bodyLen          = (body != NULL) ? (int) strlen(body) : 0;
    req.connectTimeoutMs = 5000;
    req.requestTimeoutMs = requestTimeoutMs;
    resp.allocP          = &corRest.kalloc;

    pluginP->send(&req, &resp);

    if (resp.error != 0)
    {
      xP->error = (strstr(resp.errorDetail, "imeout") != NULL) ? SE_EXCHANGE_TIMEOUT : SE_EXCHANGE_UNREACHABLE;
      snprintf(xP->detail, sizeof(xP->detail), "%s", resp.errorDetail);
      return;
    }

    xP->status = resp.statusCode;
    xP->bodyP  = forwardingBody(&resp);
    return;
  }

  CorRestClientRequest  req;
  CorRestClientResponse resp;

  corRestClientRequestInit(&req, verb, url, NULL);
  corRestClientRequestHeader(&req, "Service-Execution", execId);
  ldTenantHeaderAdd(&req, corNgsild.tenantName);

  if (body != NULL)
  {
    corRestClientRequestHeader(&req, "Content-Type", "application/json");
    corRestClientRequestBody(&req, body, strlen(body));
  }

  corRestClientRequestTimeout(&req, 5000, requestTimeoutMs);
  corRestClientSend(&req, &resp);

  if (resp.error == CORR_ERR_TIMEOUT)
    xP->error = SE_EXCHANGE_TIMEOUT;
  else if (resp.error != 0)
  {
    xP->error = SE_EXCHANGE_UNREACHABLE;
    snprintf(xP->detail, sizeof(xP->detail), "%s", resp.errorDetail);
  }
  else
  {
    xP->status = resp.statusCode;
    xP->bodyP  = responseBody(&resp);
  }

  corRestClientResponseCleanup(&resp);
}



// -----------------------------------------------------------------------------
//
// forward - a request to the executor: POST the invocation, or DELETE (cancel)
//
// http(s):// or cor:// - the same requests either way (doc/service-execution.md, "An HTTP executor"); any
// other scheme is a bridge's. The endpoint may carry a query (another coraine as the executor: ?reportTo=):
// the cancel's execution id goes in the path, before it.
//
static void forward(CorNode* execP, bool cancel, SeForward* resultP)
{
  const char* endpoint  = str(execP, "_endpoint");
  const char* execId    = str(execP, "id");
  int64_t     timeoutNs = num(execP, "_timeoutNs");
  char        url[2048];
  char*       body      = NULL;

  memset(resultP, 0, sizeof(SeForward));

  if ((ldNotifyIsHttp(endpoint) == false) && (strncmp(endpoint, "cor://", 6) != 0))
  {
    forwardBridge(execP, cancel, resultP);
    return;
  }

  seRemoteExecutors = true;                           // a registration stored before a restart: from now on

  if (cancel == true)
  {
    const char* query   = strchr(endpoint, '?');
    int         pathLen = (query != NULL) ? (int) (query - endpoint) : (int) strlen(endpoint);
    const char* slash   = ((pathLen > 0) && (endpoint[pathLen - 1] == '/')) ? "" : "/";

    snprintf(url, sizeof(url), "%.*s%s%s%s", pathLen, endpoint, slash, execId, (query != NULL) ? query : "");
  }
  else
  {
    char why[512];

    snprintf(url, sizeof(url), "%s", endpoint);

    if ((body = invocationBody(execP, why, sizeof(why))) == NULL)
    {
      resultP->clientStatus = 400;
      resultP->errorP       = problem(LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", 400, why);
      return;
    }
  }

  SeExchange x;

  exchange(url, (cancel == true) ? CorVerbDelete : CorVerbPost, execId, body, (int) (timeoutNs / 1000000), &x);

  char shown[2048];
  COR_T(CtService, "Service Execution '%s': %s %s - %d (error %d)", execId, (cancel == true) ? "DELETE" : "POST", corUriPasswordMask(url, shown, sizeof(shown)), x.status, x.error);

  if ((x.error == 0) && (x.status >= 200) && (x.status <= 299))
  {
    resultP->accepted = true;
    resultP->outputP  = x.bodyP;
  }
  else if (x.error == SE_EXCHANGE_TIMEOUT)
  {
    resultP->clientStatus = 504;
    resultP->errorP       = problem(LD_ERROR_INTERNAL_ERROR, "Executor Timeout", 504, "the service's executor did not answer in time");
  }
  else if (x.error != 0)
  {
    resultP->clientStatus = 503;
    resultP->errorP       = problem(LD_ERROR_INTERNAL_ERROR, "Executor Unreachable", 503, (x.detail[0] != 0) ? x.detail : "the service's executor could not be reached");
  }
  else
  {
    //
    // The executor refused: its ProblemDetails (else one made of its answer); a 4xx is the client's
    // to see as it is, anything else is a 502
    //
    resultP->clientStatus = ((x.status >= 400) && (x.status <= 499)) ? x.status : 502;

    if ((x.bodyP != NULL) && (x.bodyP->type == CorObject) && (corTreeLookup(x.bodyP, "type") != NULL))
      resultP->errorP = x.bodyP;
    else
    {
      char detail[256];

      snprintf(detail, sizeof(detail), "the service's executor answered %d", x.status);
      resultP->errorP = problem(LD_ERROR_INTERNAL_ERROR, "Executor Refused", x.status, detail);
    }
  }
}



// -----------------------------------------------------------------------------
//
// The report senders - the reports go out on threads of their own, never on the thread that stored
// the change: that is often the very request the other broker is waiting for (its POST, its cancel),
// and a report sent from it would wait for that broker while that broker waits for us - with one
// worker over there (a store that never waits, one core), until the report timed out.
//
// SE_REPORT_SENDERS threads, an execution always on the same one (its id hashed): its reports go in
// the order they were made, and one not sent yet is replaced by the execution's next (reportQueue).
//
#define SE_REPORT_SENDERS 4

typedef struct SeReport
{
  struct SeReport* next;
  Tenant*          tenantP;
  char*            url;
  char*            execId;
  char*            body;
} SeReport;

typedef struct SeReportQ
{
  pthread_mutex_t  mutex;
  pthread_cond_t   cond;
  SeReport*        head;
  SeReport*        tail;
} SeReportQ;

static SeReportQ      reportQV[SE_REPORT_SENDERS];
static pthread_once_t reportOnce = PTHREAD_ONCE_INIT;



static void* reportSender(void* arg)
{
  SeReportQ* qP = (SeReportQ*) arg;

  for (;;)
  {
    pthread_mutex_lock(&qP->mutex);
    while (qP->head == NULL)
      pthread_cond_wait(&qP->cond, &qP->mutex);

    SeReport* rP = qP->head;

    qP->head = rP->next;
    if (qP->head == NULL)
      qP->tail = NULL;
    pthread_mutex_unlock(&qP->mutex);

    bridgeThreadBind(rP->tenantP);                    // an arena and the tenant, for this thread

    SeExchange x;

    exchange(rP->url, CorVerbPatch, rP->execId, rP->body, 10000, &x);

    if (((x.error != 0) || (x.status < 200) || (x.status > 299)) && (x.status != 409))   // 409: ended over there (cancelled, timed out)
    {
      char shown[2048];

      COR_W("Service Execution '%s': report to %s FAILED (%d%s%s)", rP->execId, corUriPasswordMask(rP->url, shown, sizeof(shown)), x.status, (x.detail[0] != 0) ? ", " : "", x.detail);
    }

    free(rP->url);
    free(rP->execId);
    free(rP->body);
    free(rP);
  }

  return NULL;
}



static void reportSendersStart(void)
{
  for (int ix = 0; ix < SE_REPORT_SENDERS; ix++)
  {
    pthread_t tid;

    pthread_mutex_init(&reportQV[ix].mutex, NULL);
    pthread_cond_init(&reportQV[ix].cond, NULL);
    reportQV[ix].head = NULL;
    reportQV[ix].tail = NULL;

    if (pthread_create(&tid, NULL, reportSender, &reportQV[ix]) == 0)
    {
      pthread_detach(tid);
    }
    else
      COR_E("Service Execution: cannot start a report sender thread");
  }
}



static void reportQueue(const char* url, const char* execId, const char* body)
{
  SeReport* rP = (SeReport*) calloc(1, sizeof(SeReport));

  pthread_once(&reportOnce, reportSendersStart);

  if (rP == NULL)
    return;

  rP->tenantP = (Tenant*) corNgsild.tenantP;
  rP->url     = strdup(url);
  rP->execId  = strdup(execId);
  rP->body    = strdup(body);

  unsigned int hash = 5381;

  for (const char* cP = execId; *cP != 0; cP++)
    hash = hash * 33 + (unsigned char) *cP;

  SeReportQ* qP = &reportQV[hash % SE_REPORT_SENDERS];

  pthread_mutex_lock(&qP->mutex);

  //
  // The execution's previous report not sent yet (the last one queued): this one replaces it - a report
  // is the execution as it is now (status, progress, output, error), so the newer says all the older did
  //
  if ((qP->tail != NULL) && (strcmp(qP->tail->execId, rP->execId) == 0))
  {
    free(qP->tail->body);
    qP->tail->body = rP->body;
    pthread_mutex_unlock(&qP->mutex);
    free(rP->url);
    free(rP->execId);
    free(rP);
    return;
  }

  if (qP->tail != NULL)
    qP->tail->next = rP;
  else
    qP->head = rP;
  qP->tail = rP;
  pthread_cond_signal(&qP->cond);
  pthread_mutex_unlock(&qP->mutex);
}



// -----------------------------------------------------------------------------
//
// report - this broker is another broker's executor (seExecuteFor): an asynchronous execution's change
// reported to it, as an HTTP executor reports (doc/service-execution.md, "An HTTP executor"):
//
//   PATCH <reportTo>/ngsi-ld/v1/services/{id}     Service-Execution: {id}
//   { "executionStatus": ..., "executionProgress": ..., "executionOutput": ..., "executionError": ... }
//
// The execution has the caller's id. Over http(s):// or cor://, as reportTo says - on a report sender
// thread (reportQueue). A synchronous one is not reported: its answer is the report.
//
static void report(CorNode* execP)
{
  const char* reportTo = str(execP, "_reportTo");
  const char* execId   = str(execP, "id");
  const char* mode     = str(execP, "_mode");
  const char* status   = str(execP, "executionStatus");

  if ((mode == NULL) || (strcmp(mode, "asynchronous") != 0) || (status == NULL) || (strcmp(status, "pending") == 0))
    return;

  if ((strcmp(status, "executing") == 0) && (corTreeLookup(execP, "executionProgress") == NULL))
    return;                                           // nothing new: the other broker set it executing as it handed it over

  CorNode* bodyP = corTreeObject(corRest.kallocP, NULL);
  char     url[2048];
  int      len   = (int) strlen(reportTo);

  corTreeChildAdd(bodyP, corTreeString(corRest.kallocP, "executionStatus", status));

  const char* memberV[] = { "executionProgress", "executionOutput", "executionError" };

  for (unsigned int ix = 0; ix < sizeof(memberV) / sizeof(memberV[0]); ix++)
  {
    CorNode* mP = corTreeLookup(execP, memberV[ix]);

    if (mP != NULL)
      corTreeChildAdd(bodyP, corTreeClone(corRest.kallocP, mP));
  }

  int   size = corJsonFastRenderSize(bodyP) + 1;
  char* body = (char*) corAlloc(&corRest.kalloc, size);

  corJsonFastRender(bodyP, body);

  snprintf(url, sizeof(url), "%.*s/ngsi-ld/v1/services/%s", ((len > 0) && (reportTo[len - 1] == '/')) ? len - 1 : len, reportTo, execId);

  reportQueue(url, execId, body);
}



// -----------------------------------------------------------------------------
//
// answerError - the request answered with the execution's ProblemDetails
//
static void answerError(int status, CorNode* errorP, const char* execId)
{
  CorNode* detailP = corTreeLookup(errorP, "detail");
  CorNode* titleP  = corTreeLookup(errorP, "title");
  CorNode* typeP   = corTreeLookup(errorP, "type");

  ldError(status,
          ((typeP != NULL) && (typeP->type == CorString)) ? typeP->value.s : LD_ERROR_INTERNAL_ERROR,
          ((titleP != NULL) && (titleP->type == CorString)) ? titleP->value.s : "Service Execution Failed",
          "%s", ((detailP != NULL) && (detailP->type == CorString)) ? detailP->value.s : "the service's execution failed");

  corRestOutHeaderAdd("Service-Execution", execId);
}



// -----------------------------------------------------------------------------
//
// seExecutionBuild - a simple execution, pending, not stored: the entity, its service, the input checked
//
// NULL: refused - *statusP and why say how (404 entity / service, 409 a service offered twice, 400 input).
//
CorNode* seExecutionBuild(const char* entityId, const char* serviceName, CorNode* inputP, CorNode* notificationP, int* statusP, char* why, int whySize)
{
  Tenant*  tenantP = (Tenant*) corNgsild.tenantP;
  CorNode* entityP = NULL;

  *statusP = 0;

  if (db.entityRetrieve(tenantP, entityId, &entityP) != DB_OK)
  {
    *statusP = 404;
    snprintf(why, whySize, "entity '%s' not found", entityId);
    return NULL;
  }

  CorNode* regP = serviceFind(tenantP, entityId, entityP, serviceName, statusP, why, whySize);

  if ((regP == NULL) && (*statusP == 409))
    return NULL;                                      // offered twice - why says by which registrations

  if (regP == NULL)
  {
    *statusP = 404;
    snprintf(why, whySize, "entity '%s' has no service '%s'", entityId, corLdCompact(corNgsild.contextP, serviceName, &corRest.kalloc));
    return NULL;
  }

  CorNode*    siP    = corTreeLookup(regP, "serviceInformation");
  const char* mode   = str(siP, "mode");
  bool        sync   = (mode == NULL) || (strcmp(mode, "synchronous") == 0);
  CorNode*    schema = corTreeLookup(siP, "inputSchema");
  char        schemaWhy[400];

  if (inputP == NULL)
    inputP = corTreeObject(corRest.kallocP, NULL);

  if (seJsonSchemaCheck(schema, inputP, "", schemaWhy, sizeof(schemaWhy)) == false)
  {
    *statusP = 400;
    snprintf(why, whySize, "the service's input: %s", schemaWhy);
    return NULL;
  }

  const char* timeout   = str(regP, "executionTimeout");
  int64_t     timeoutNs = (timeout != NULL) ? ldIso8601DurationParseNs(timeout) : ((sync == true) ? 30LL * 1000000000LL : 3600LL * 1000000000LL);
  char*       execId    = ldIdGenerate(&corRest.kalloc, "ServiceExecution");
  CorNode*    execP     = corTreeObject(corRest.kallocP, NULL);
  CorNode*    typeP     = corTreeLookup(entityP, "type");
  const char* type      = ((typeP != NULL) && (typeP->type == CorString)) ? typeP->value.s : (((typeP != NULL) && (typeP->type == CorArray) && (typeP->value.head != NULL)) ? typeP->value.head->value.s : "");

  if (notificationP == NULL)
    notificationP = corTreeLookup(regP, "notification");

  corTreeChildAdd(execP, corTreeString(corRest.kallocP, "id",               execId));
  corTreeChildAdd(execP, corTreeString(corRest.kallocP, "type",             "ServiceExecution"));
  corTreeChildAdd(execP, corTreeString(corRest.kallocP, "entityId",         entityId));
  corTreeChildAdd(execP, corTreeString(corRest.kallocP, "entityType",       type));
  corTreeChildAdd(execP, corTreeString(corRest.kallocP, "serviceName",      serviceName));
  inputP = corTreeClone(corRest.kallocP, inputP);
  inputP->name = (char*) "executionInput";
  corTreeChildAdd(execP, inputP);
  corTreeChildAdd(execP, corTreeString(corRest.kallocP, "executionStatus",  "pending"));

  if (notificationP != NULL)
  {
    notificationP = corTreeClone(corRest.kallocP, notificationP);
    notificationP->name = (char*) "notification";
    corTreeChildAdd(execP, notificationP);
  }

  corTreeChildAdd(execP, corTreeString(corRest.kallocP,  "_registration", str(regP, "id")));
  corTreeChildAdd(execP, corTreeString(corRest.kallocP,  "_endpoint",     str(regP, "endpoint")));

  corTreeChildAdd(execP, corTreeString(corRest.kallocP,  "_mode",         (sync == true) ? "synchronous" : "asynchronous"));
  corTreeChildAdd(execP, corTreeInteger(corRest.kallocP, "_timeoutNs",    timeoutNs));

  if ((corNgsild.contextP != NULL) && (corNgsild.contextP->url != NULL))
    corTreeChildAdd(execP, corTreeString(corRest.kallocP, "_context", corNgsild.contextP->url));

  ldSysTimestampCreate(execP);

  return execP;
}



// -----------------------------------------------------------------------------
//
// seExecutionStart - a stored, pending simple execution handed to its executor; its outcome stored
//
// An asynchronous execution is executing - stored, and notified - BEFORE the hand-off: the executor
// reports on threads (requests) of its own, possibly before forward() returns, and a store after it
// would overwrite what it reported. Once it accepted, the record is the executor's; only a refusal
// (which no report follows) is stored here. A synchronous one is stored with its outcome.
//
// *resultP: the hand-off's outcome (the outputP and errorP in corRest.kalloc).
//
void seExecutionStart(CorNode* execP, SeForward* resultP)
{
  Tenant*     tenantP = (Tenant*) corNgsild.tenantP;
  const char* mode    = str(execP, "_mode");
  bool        sync    = (mode == NULL) || (strcmp(mode, "synchronous") == 0);

  timestamp(execP, "executionStartedAt", "_startedNs", nowNs());

  if (sync == false)
  {
    member(execP, corTreeString(corRest.kallocP, "executionStatus", "executing"));
    seExecutionStore(tenantP, execP);
  }

  forward(execP, false, resultP);

  if (resultP->accepted == false)
  {
    finish(execP, "failed", NULL, resultP->errorP);
    seExecutionStore(tenantP, execP);
  }
  else if (sync == true)
  {
    finish(execP, "completed", resultP->outputP, NULL);
    seExecutionStore(tenantP, execP);
  }
}



// -----------------------------------------------------------------------------
//
// seExecute -
//
static bool execute(SeOrigin origin, const char* entityId, const char* serviceName, CorNode* inputP, CorNode* notificationP, const char* callerId, const char* reportTo)
{
  Tenant* tenantP = (Tenant*) corNgsild.tenantP;
  int     status  = 0;
  char    why[512];

  if ((db.docCreate == NULL) || (db.entityRetrieve == NULL))
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Operation Not Supported", "the database plugin keeps no Service Executions");
    return true;
  }

  CorNode* execP = seExecutionBuild(entityId, serviceName, inputP, notificationP, &status, why, sizeof(why));

  if (execP == NULL)
  {
    if (status == 404)
      ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "%s", why);
    else if (status == 409)
      ldError(409, LD_ERROR_CONFLICT, "Conflict", "%s", why);
    else
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "%s", why);
    return true;
  }

  const char* mode = str(execP, "_mode");
  bool        sync = (strcmp(mode, "synchronous") == 0);

  if (callerId != NULL)
  {
    //
    // Another broker's execution (seExecuteFor): ours has its id, and its asynchronous one reports there
    //
    CorNode* existingP = NULL;

    if ((db.docRetrieve != NULL) && (db.docRetrieve(tenantP, "serviceExecutions", callerId, &existingP) == DB_OK))
    {
      ldError(409, LD_ERROR_CONFLICT, "Conflict", "Service Execution '%s' is already here", callerId);
      return true;
    }

    if ((sync == false) && (reportTo == NULL))
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "an asynchronous service reports to the broker that asked for it: the endpoint's '?reportTo=' (its http(s):// or cor:// address)");
      return true;
    }

    member(execP, corTreeString(corRest.kallocP, "id", callerId));

    if (reportTo != NULL)
      corTreeChildAdd(execP, corTreeString(corRest.kallocP, "_reportTo", reportTo));
  }

  const char* execId = str(execP, "id");

  if (db.docCreate(tenantP, "serviceExecutions", execId, execP) != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error creating the Service Execution");
    return true;
  }

  SeForward result;

  seExecutionStart(execP, &result);

  //
  // The answer
  //
  if (result.accepted == false)
  {
    answerError(result.clientStatus, result.errorP, execId);
    return true;
  }

  int   size     = 64 + strlen(execId);
  char* location = (char*) corAlloc(&corRest.kalloc, size);

  snprintf(location, size, "/ngsi-ld/v1/services/%s", execId);
  corRestOutHeaderAdd("Service-Execution", execId);

  if (origin == SeCreate)
  {
    corRestOutHeaderAdd("Location", location);
    corRest.out.httpStatusCode = 201;
  }
  else if ((origin == SeExecutor) && (sync == false))
    corRest.out.httpStatusCode = 202;                 // accepted - the reports follow (report())
  else if (sync == true)
  {
    //
    // A synchronous invocation answers with the service's result
    //
    corRest.out.httpStatusCode = 200;

    if (result.outputP != NULL)
    {
      CorNode* outP = corTreeClone(corRest.kallocP, result.outputP);

      outP->name               = NULL;
      corNgsild.rawResponse    = true;
      corRest.out.responseTree = outP;
    }
  }
  else
  {
    corRestOutHeaderAdd("Location", location);
    corRest.out.httpStatusCode = 202;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// seExecute -
//
bool seExecute(SeOrigin origin, const char* entityId, const char* serviceName, CorNode* inputP, CorNode* notificationP)
{
  return execute(origin, entityId, serviceName, inputP, notificationP, NULL, NULL);
}



// -----------------------------------------------------------------------------
//
// seExecuteFor - this broker as another broker's executor: callerId is that broker's execution, which
// ours takes as its id; an asynchronous one reports to reportTo (report())
//
bool seExecuteFor(const char* entityId, const char* serviceName, CorNode* inputP, const char* callerId, const char* reportTo)
{
  return execute(SeExecutor, entityId, serviceName, inputP, NULL, callerId, reportTo);
}



// -----------------------------------------------------------------------------
//
// seExecutionRetrieve -
//
CorNode* seExecutionRetrieve(const char* execId)
{
  CorNode* execP = NULL;
  int      r     = (db.docRetrieve != NULL) ? db.docRetrieve((Tenant*) corNgsild.tenantP, "serviceExecutions", execId, &execP) : DB_NOT_FOUND;

  if (r == DB_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "Service Execution '%s' not found", execId);
    return NULL;
  }
  else if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error retrieving Service Execution '%s'", execId);
    return NULL;
  }

  return execP;
}



// -----------------------------------------------------------------------------
//
// seExecutionUpdate -
//
bool seExecutionUpdate(CorNode* execP, CorNode* updateP)
{
  const char* status    = str(execP, "executionStatus");
  const char* newStatus = NULL;

  if (terminal(status))
  {
    ldError(409, LD_ERROR_CONFLICT, "Conflict", "the Service Execution is %s - it changes no more", status);
    return true;
  }

  for (CorNode* mP = updateP->value.head; mP != NULL; mP = mP->next)
  {
    if (strcmp(mP->name, "executionStatus") == 0)
    {
      if ((mP->type != CorString) ||
          ((strcmp(mP->value.s, "executing") != 0) && (terminal(mP->value.s) == false)))
      {
        ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'executionStatus' is executing, completed, failed or cancelled");
        return true;
      }
      newStatus = mP->value.s;
    }
    else if ((strcmp(mP->name, "executionProgress") != 0) && (strcmp(mP->name, "executionOutput") != 0) && (strcmp(mP->name, "executionError") != 0))
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "'%s' - an executor reports executionStatus, executionProgress, executionOutput and executionError",
              corLdCompact(corNgsild.contextP, mP->name, &corRest.kalloc));
      return true;
    }
  }

  CorNode* errorP = corTreeLookup(updateP, "executionError");

  if ((newStatus != NULL) && (strcmp(newStatus, "failed") == 0) && ((errorP == NULL) || (errorP->type != CorObject)))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request Data", "a failed Service Execution needs its 'executionError' - a ProblemDetails");
    return true;
  }

  //
  // Applied: the members as they come, the status last
  //
  CorNode* mP = updateP->value.head;

  while (mP != NULL)
  {
    CorNode* nextP = mP->next;

    corTreeChildRemove(updateP, mP);

    if (strcmp(mP->name, "executionStatus") != 0)
      member(execP, mP);

    mP = nextP;
  }

  if ((newStatus != NULL) && terminal(newStatus))
    finish(execP, newStatus, NULL, NULL);
  else if (newStatus != NULL)
    member(execP, corTreeString(corRest.kallocP, "executionStatus", newStatus));

  if (seExecutionStore((Tenant*) corNgsild.tenantP, execP) == false)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error updating the Service Execution");
    return true;
  }

  corRest.out.httpStatusCode = 204;
  return true;
}



// -----------------------------------------------------------------------------
//
// seExecutionCancel -
//
int seExecutionCancelOne(CorNode* execP, SeForward* resultP)
{
  const char* status = str(execP, "executionStatus");

  memset(resultP, 0, sizeof(SeForward));

  if (terminal(status))
    return 204;

  if (strcmp(status, "executing") == 0)
  {
    forward(execP, true, resultP);

    if (resultP->accepted == false)
      return resultP->clientStatus;
  }

  finish(execP, "cancelled", NULL, NULL);

  return (seExecutionStore((Tenant*) corNgsild.tenantP, execP) == true) ? 204 : 500;
}



// -----------------------------------------------------------------------------
//
// seExecutionCancel -
//
bool seExecutionCancel(CorNode* execP)
{
  Tenant*     tenantP = (Tenant*) corNgsild.tenantP;
  const char* status  = str(execP, "executionStatus");
  const char* execId  = str(execP, "id");
  const char* type    = str(execP, "type");

  if ((type != NULL) && (strcmp(type, "ServiceExecution") != 0))
    return seCombinedCancel(execP);

  if (terminal(status))
  {
    //
    // Finished: the record goes (cancel and delete are two things - report § 8.1.1)
    //
    if (db.docDelete(tenantP, "serviceExecutions", execId) != DB_OK)
      ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error deleting Service Execution '%s'", execId);
    else
      corRest.out.httpStatusCode = 204;

    return true;
  }

  if (strcmp(status, "executing") == 0)
  {
    SeForward result;

    forward(execP, true, &result);

    if (result.accepted == false)
    {
      if (result.clientStatus == 409)
        ldError(409, LD_ERROR_CONFLICT, "Conflict", "the service's executor cannot cancel Service Execution '%s' - it goes on", execId);
      else
        answerError(result.clientStatus, result.errorP, execId);

      return true;
    }
  }

  finish(execP, "cancelled", NULL, NULL);

  if (seExecutionStore(tenantP, execP) == false)
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error updating Service Execution '%s'", execId);
  else
    corRest.out.httpStatusCode = 204;

  return true;
}



// -----------------------------------------------------------------------------
//
// Ended - an ended top-level execution, for the seeding: its id and when it ended
//
typedef struct Ended
{
  const char* id;
  int64_t     endedNs;
} Ended;

static int endedOrder(const void* a, const void* b)
{
  int64_t x = ((const Ended*) a)->endedNs;
  int64_t y = ((const Ended*) b)->endedNs;

  return (x < y) ? -1 : (x > y) ? 1 : 0;
}



// -----------------------------------------------------------------------------
//
// seExecutionSweep -
//
// Every 5 s: a running execution past its executionTimeout fails; an ended TOP-LEVEL one older than
// --executionRetention goes, with its tree (a child goes with its parent, never before it - a parent still
// running shows its children).
//
// The first sweep of a tenant also counts what ended before the broker started (stored by an earlier run),
// oldest first, so that --executionMaxEnded holds from the start - a store left with more than the cap is
// cut down to it here. What ended since the start was counted as it ended.
//
// With the cap, the store holds the running executions and at most --executionMaxEnded ended ones (and their
// children): the copy this takes stays small.
//
void seExecutionSweep(Tenant* tenantP, int64_t now)
{
  CorNode* execsP = NULL;

  if ((db.docQuery == NULL) || (db.docQuery(tenantP, "serviceExecutions", &execsP) != DB_OK))
    return;

  bool   seed    = (seRetentionMaxEnded > 0) && (seRetentionSeeded(tenantP) == false);
  int    endedN  = 0;
  Ended* endedV  = NULL;

  if (seed == true)
  {
    int n = 0;

    for (CorNode* execP = execsP->value.head; execP != NULL; execP = execP->next)
      ++n;

    endedV = (Ended*) corAlloc(corRest.kallocP, (n + 1) * sizeof(Ended));
  }

  for (CorNode* execP = execsP->value.head; execP != NULL; execP = execP->next)
  {
    const char* status   = str(execP, "executionStatus");
    bool        topLevel = (str(execP, "_parent") == NULL);

    if (terminal(status))
    {
      if (topLevel == false)
        continue;

      if (now - num(execP, "_endedNs") > seExecutionRetentionNs)
        seCombinedDeleteTree(tenantP, execP);
      else if ((endedV != NULL) && (num(execP, "_endedNs") < retentionFromNs))
      {
        endedV[endedN].id      = str(execP, "id");
        endedV[endedN].endedNs = num(execP, "_endedNs");
        ++endedN;
      }
    }
    else if ((num(execP, "_startedNs") > 0) && (now - num(execP, "_startedNs") > num(execP, "_timeoutNs")))
    {
      finish(execP, "failed", NULL, problem(LD_ERROR_INTERNAL_ERROR, "Execution Timeout", 504, "the service's execution did not end within its executionTimeout"));
      seExecutionStore(tenantP, execP);
    }
  }

  if (endedN > 0)
  {
    qsort(endedV, endedN, sizeof(Ended), endedOrder);

    for (int ix = 0; ix < endedN; ix++)
      retain(tenantP, endedV[ix].id);
  }
}



// -----------------------------------------------------------------------------
//
// seExecutionRetentionSet - --executionRetention (seconds) and --executionMaxEnded, at start
//
void seExecutionRetentionSet(int retentionS, int maxEnded)
{
  seExecutionRetentionNs = (int64_t) retentionS * 1000000000LL;
  seRetentionMaxEnded    = maxEnded;
  retentionFromNs        = nowNs();
}



// -----------------------------------------------------------------------------
//
// seExecutionTick -
//
// The loop's thread has its own corRest.kalloc, reset after every tick - what the sweep allocates goes
// with it. The tenant is set as a request's would be: its notifications carry it.
//
void seExecutionTick(void* ctx, uint64_t now, CorAlloc* kaP)
{
  static int calls = 0;

  (void) ctx;
  (void) kaP;

  if ((++calls % 5) != 0)
    return;

  for (Tenant* tP = &tenant0; tP != NULL; tP = (tP == &tenant0) ? __atomic_load_n(&tenantList, __ATOMIC_ACQUIRE) : tP->next)
  {
    corNgsild.tenantP    = tP;
    corNgsild.tenantName = (tP->name[0] != 0) ? tP->name : NULL;

    seExecutionSweep(tP, (int64_t) now);
  }

  corNgsild.tenantP    = NULL;
  corNgsild.tenantName = NULL;
}



// -----------------------------------------------------------------------------
//
// seExecutionApplyBridge - an asynchronous execution's report from its bridge (bridgeServiceApplySet)
//
// On the plugin's thread, bound as the execution's tenant (bridgeThreadBind).
//
void seExecutionApplyBridge(const char* executionId, const char* status, const char* progressJson, const char* outputJson, const char* errorJson)
{
  Tenant*  tenantP = (Tenant*) corNgsild.tenantP;
  CorNode* execP   = NULL;

  if ((db.docRetrieve == NULL) || (db.docRetrieve(tenantP, "serviceExecutions", executionId, &execP) != DB_OK))
  {
    COR_W("Service Execution '%s': a bridge's report on an execution that is not there", executionId);
    return;
  }

  if (terminal(str(execP, "executionStatus")))
    return;                                           // ended (cancelled, timed out) - a late report changes nothing

  CorNode* progressP = jsonTree(progressJson);

  if (progressP != NULL)
  {
    progressP->name = (char*) "executionProgress";
    member(execP, progressP);
  }

  if ((status != NULL) && (strcmp(status, "completed") == 0))
    finish(execP, "completed", jsonTree(outputJson), NULL);
  else if ((status != NULL) && (strcmp(status, "failed") == 0))
  {
    CorNode* errorP = jsonTree(errorJson);

    if ((errorP == NULL) || (errorP->type != CorObject))
      errorP = problem(LD_ERROR_INTERNAL_ERROR, "Execution Failed", 500, "the service's bridge reported a failure");

    finish(execP, "failed", NULL, errorP);
  }
  else if (status != NULL)
    member(execP, corTreeString(corRest.kallocP, "executionStatus", status));

  seExecutionStore(tenantP, execP);
}

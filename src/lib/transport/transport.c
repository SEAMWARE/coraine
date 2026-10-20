//
// FILE            transport.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <pthread.h>                                 // pthread_mutex_t
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // malloc, realloc, free, strtol
#include <string.h>                                  // memset, strcmp, strncmp, strlen, strrchr, strncpy, strtok_r
#include <strings.h>                                 // strcasecmp

#include <stdint.h>                                 // uint8_t, uint64_t
#include <sys/random.h>                             // getrandom
#include <unistd.h>                                 // getpid
#include <time.h>                                   // time

#include "corPlugin/corPlugin.h"                     // corPluginOpen, corPluginResolve, corPluginBaseDir
#include "corLog/corLog.h"                           // COR_I, COR_W, COR_E
#include "corAlloc/CorAlloc.h"                       // CorAlloc
#include "corAlloc/corAlloc.h"                       // corAlloc, corAllocStrdup
#include "corAlloc/corAllocBufferInit.h"             // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"            // corAllocBufferReset
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corJson/CorJson.h"                         // CorJson
#include "corJson/corJsonCreate.h"                   // corJsonCreate
#include "corJson/corJsonParse.h"                    // corJsonParse
#include "corJson/corJsonRender.h"                   // corJsonFastRender
#include "corJson/corJsonRenderSize.h"               // corJsonFastRenderSize
#include "corRest/corRest.h"                         // corRest
#include "corRest/CorRestKeyValue.h"                 // CorRestKeyValue
#include "corRest/corRestHooks.h"                    // corRestSetUpgradeHook, CorRestUpgradeTake
#include "corRest/corRestOutHeader.h"                // corRestOutHeaderAdd
#include "corRest/corRestProblem.h"                  // corRestProblem
#include "corRest/corRestRun.h"                      // corRestRunJson

#if COR_FEATURE_AUTH
#include "auth/auth.h"                               // authTrustNext
#endif
#include "corNgsild/LdSubCache.h"                    // LdSubCache, LdSubCacheItem
#include "corNgsild/ldSubCache.h"                    // ldSubCacheRdLock, ldSubCacheUnlock
#include "corNgsild/LdPernotCache.h"                 // LdPernotCache, LdPernotItem
#include "corNgsild/ldPernotCache.h"                 // ldPernotCacheRdLock, ldPernotCacheUnlock
#include "db/Tenant.h"                               // Tenant, tenant0, tenantList, tenantApiName
#include "plugin/TransportDriver.h"                  // TransportDriver, TransportHost
#include "transport/transport.h"                     // Own interface



// -----------------------------------------------------------------------------
//
// The loaded transports
//
#define TRANSPORTS_MAX   4

static TransportDriver  transports[TRANSPORTS_MAX];
static int              transportCount = 0;



// -----------------------------------------------------------------------------
//
// A connection's id, as a subscription's endpoint names it (doc/websocket.md § 4). Version 1: one
// transport of connections (ws), and these ids are its.
//
#define CONN_PREFIX      "urn:ngsi-ld:WebSocket:"
#define CONN_PREFIX_LEN  22
#define WS_PATH          "/ngsi-ld/v1/ws"
#define SUBS_PATH        "/ngsi-ld/v1/subscriptions"
#define CSUBS_PATH       "/ngsi-ld/v1/csourceSubscriptions"



// -----------------------------------------------------------------------------
//
// Conn - an open connection, and the subscriptions made to notify it (deleted when it closes)
//
typedef struct Owned
{
  char*  path;                                       // as its Location said: /ngsi-ld/v1/subscriptions/<id>
  char*  tenant;                                     // NULL: the default tenant
} Owned;

typedef struct Conn
{
  int               connId;
  TransportDriver*  driverP;
  Owned*            ownV;                            // the subscriptions made to notify it - deleted when it closes
  int               ownN;
  int               ownSize;
  bool              running;                         // a request of its own runs, on runThread (hostMessage)
  pthread_t         runThread;
  struct Conn*      next;
} Conn;



// -----------------------------------------------------------------------------
//
// bootId - this broker's run, in every connection's id: urn:ngsi-ld:WebSocket:<bootId>-<n>
//
// The <n> start again at 1 with every run, and every instance on one store has its own. An id of another
// run - a subscription a killed broker never deleted - or of another instance names no connection here:
// nothing is ever delivered to whoever is connection <n> now. Random, set by transportInit.
//
static char bootId[17];

static pthread_mutex_t  connMutex = PTHREAD_MUTEX_INITIALIZER;
static Conn*            connList  = NULL;
static TransportHost    host;



// -----------------------------------------------------------------------------
//
// connFind - under connMutex
//
static Conn* connFind(int connId)
{
  for (Conn* cP = connList; cP != NULL; cP = cP->next)
  {
    if (cP->connId == connId)
      return cP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// runMark - the connection runs a request of its own on this thread (on), or no longer (off)
//
static void runMark(int connId, bool on)
{
  pthread_mutex_lock(&connMutex);

  Conn* cP = connFind(connId);

  if (cP != NULL)
  {
    cP->running   = on;
    cP->runThread = pthread_self();
  }

  pthread_mutex_unlock(&connMutex);
}



// -----------------------------------------------------------------------------
//
// connIdOf - the connection a URI names (urn:ngsi-ld:WebSocket:<bootId>-<n>), -1 if none - and none for an id
// of another run or another instance (bootId)
//
static int connIdOf(const char* uri)
{
  if ((uri == NULL) || (strncmp(uri, CONN_PREFIX, CONN_PREFIX_LEN) != 0))
    return -1;

  const char* p = &uri[CONN_PREFIX_LEN];

  if ((strncmp(p, bootId, 16) != 0) || (p[16] != '-'))
    return -1;

  char* end = NULL;
  long  n   = strtol(&p[17], &end, 10);

  return ((end != NULL) && (end != &p[17]) && (*end == 0) && (n >= 0)) ? (int) n : -1;
}



// -----------------------------------------------------------------------------
//
// ownTrack / ownUntrack - a subscription (or a context source subscription) made, over this connection, to
// notify it - in its tenant - or deleted, or changed to notify elsewhere
//
static bool tenantSame(const char* a, const char* b)
{
  return ((a == NULL) && (b == NULL)) || ((a != NULL) && (b != NULL) && (strcmp(a, b) == 0));
}

static void ownTrack(int connId, const char* path, const char* tenant)
{
  pthread_mutex_lock(&connMutex);

  Conn* cP = connFind(connId);

  for (int i = 0; (cP != NULL) && (i < cP->ownN); i++)
  {
    if ((strcmp(cP->ownV[i].path, path) == 0) && tenantSame(cP->ownV[i].tenant, tenant))
      cP = NULL;                                     // tracked already: a PATCH to the endpoint it had
  }

  if (cP != NULL)
  {
    if (cP->ownN == cP->ownSize)
    {
      int    size = (cP->ownSize == 0) ? 8 : cP->ownSize * 2;
      Owned* v    = (Owned*) realloc(cP->ownV, size * sizeof(Owned));

      if (v != NULL)
      {
        cP->ownV    = v;
        cP->ownSize = size;
      }
    }

    if (cP->ownN < cP->ownSize)
    {
      cP->ownV[cP->ownN].path   = strdup(path);
      cP->ownV[cP->ownN].tenant = (tenant != NULL) ? strdup(tenant) : NULL;
      cP->ownN += 1;
    }
  }

  pthread_mutex_unlock(&connMutex);
}

static void ownUntrack(int connId, const char* path, const char* tenant)
{
  pthread_mutex_lock(&connMutex);

  Conn* cP = connFind(connId);

  for (int i = 0; (cP != NULL) && (i < cP->ownN); i++)
  {
    if ((strcmp(cP->ownV[i].path, path) == 0) && tenantSame(cP->ownV[i].tenant, tenant))
    {
      free(cP->ownV[i].path);
      free(cP->ownV[i].tenant);
      cP->ownV[i] = cP->ownV[--cP->ownN];
      break;
    }
  }

  pthread_mutex_unlock(&connMutex);
}



// -----------------------------------------------------------------------------
//
// Buf - a growing text buffer, for the envelopes
//
typedef struct Buf
{
  char*  s;
  int    len;
  int    size;
} Buf;

static void bufAdd(Buf* bP, const char* s, int n)
{
  if (bP->len + n + 1 > bP->size)
  {
    int   size = (bP->size == 0) ? 1024 : bP->size;
    char* v;

    while (bP->len + n + 1 > size)
      size *= 2;

    v = (char*) realloc(bP->s, size);
    if (v == NULL)
      return;

    bP->s    = v;
    bP->size = size;
  }

  memcpy(&bP->s[bP->len], s, n);
  bP->len += n;
  bP->s[bP->len] = 0;
}

static void bufStr(Buf* bP, const char* s)                    // a JSON string, quoted and escaped
{
  bufAdd(bP, "\"", 1);

  for (const char* p = s; (p != NULL) && (*p != 0); p++)
  {
    char esc[8];

    if ((*p == '"') || (*p == '\\'))
    {
      esc[0] = '\\';
      esc[1] = *p;
      bufAdd(bP, esc, 2);
    }
    else if ((unsigned char) *p < 0x20)
    {
      snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char) *p);
      bufAdd(bP, esc, 6);
    }
    else
      bufAdd(bP, p, 1);
  }

  bufAdd(bP, "\"", 1);
}



// -----------------------------------------------------------------------------
//
// reply - a response envelope: { "metadata": { "status", "requestId", <headers> }, "body": <body> }
//
static void reply(int connId, int status, const char* requestId, CorRestKeyValue* headerV, int headers, const char* body, int bodyLen)
{
  pthread_mutex_lock(&connMutex);
  Conn*            cP      = connFind(connId);
  TransportDriver* driverP = (cP != NULL) ? cP->driverP : NULL;
  pthread_mutex_unlock(&connMutex);

  if (driverP == NULL)
    return;

  Buf  b = { NULL, 0, 0 };
  char statusS[16];

  snprintf(statusS, sizeof(statusS), "%d", status);

  bufAdd(&b, "{\"metadata\":{\"status\":", 22);
  bufAdd(&b, statusS, strlen(statusS));

  if (requestId != NULL)
  {
    bufAdd(&b, ",\"requestId\":", 13);
    bufStr(&b, requestId);
  }

  for (int i = 0; i < headers; i++)
  {
    bufAdd(&b, ",", 1);
    bufStr(&b, headerV[i].key);
    bufAdd(&b, ":", 1);
    bufStr(&b, headerV[i].value);
  }

  bufAdd(&b, "}", 1);

  if ((body != NULL) && (bodyLen > 0))
  {
    //
    // JSON as it is; anything else - the text of /metrics - as a JSON string, so the envelope stays JSON
    //
    const char* p = body;

    while ((*p == ' ') || (*p == '\n') || (*p == '\r') || (*p == '\t'))
      ++p;

    bufAdd(&b, ",\"body\":", 8);

    if ((*p == '{') || (*p == '['))
      bufAdd(&b, body, bodyLen);
    else
    {
      char* text = (char*) malloc(bodyLen + 1);

      if (text != NULL)
      {
        memcpy(text, body, bodyLen);
        text[bodyLen] = 0;
        bufStr(&b, text);
        free(text);
      }
      else
        bufAdd(&b, "null", 4);
    }
  }

  bufAdd(&b, "}", 1);

  if (b.s != NULL)
    driverP->send(connId, b.s, b.len);

  free(b.s);
}

static void replyError(int connId, const char* requestId, int status, const char* type, const char* title, const char* detail)
{
  Buf  b = { NULL, 0, 0 };
  char statusS[16];

  snprintf(statusS, sizeof(statusS), "%d", status);

  bufAdd(&b, "{\"type\":", 8);
  bufStr(&b, type);
  bufAdd(&b, ",\"title\":", 9);
  bufStr(&b, title);
  bufAdd(&b, ",\"status\":", 10);
  bufAdd(&b, statusS, strlen(statusS));
  bufAdd(&b, ",\"detail\":", 10);
  bufStr(&b, detail);
  bufAdd(&b, "}", 1);

  CorRestKeyValue ct = { (char*) "Content-Type", (char*) "application/json" };

  reply(connId, status, requestId, &ct, 1, b.s, b.len);
  free(b.s);
}



// -----------------------------------------------------------------------------
//
// RunCtx / runRespond - corRestRunJson's response, to the connection; the subscriptions it created and
// deleted followed
//
typedef struct RunCtx
{
  int          connId;
  const char*  requestId;
  const char*  verb;
  const char*  path;
  const char*  tenant;                               // the request's - NULL: the default
  int          endpoint;                             // the body's notification.endpoint.uri: 1 this connection, 0 another, -1 none
} RunCtx;

static void runRespond(int status, CorRestKeyValue* headerV, int headers, const char* body, int bodyLen, void* ctx)
{
  RunCtx* rP = (RunCtx*) ctx;

  //
  // The subscriptions made to notify this connection - of entities and of context sources, only those
  // (a 201's Location is an entity's, a registration's ... on other paths). One that notifies elsewhere
  // (http, mqtt) is the client's, not the connection's: it stays when the connection closes.
  //
  bool        subs = (strcmp(rP->path, SUBS_PATH) == 0) || (strcmp(rP->path, CSUBS_PATH) == 0);
  char        path[512];
  const char* q    = strchr(rP->path, '?');
  int         n    = (q != NULL) ? (int) (q - rP->path) : (int) strlen(rP->path);

  if (n >= (int) sizeof(path))
    n = 0;

  memcpy(path, rP->path, n);
  path[n] = 0;

  bool one = ((strncmp(path, SUBS_PATH "/", sizeof(SUBS_PATH)) == 0) || (strncmp(path, CSUBS_PATH "/", sizeof(CSUBS_PATH)) == 0));

  if ((status == 201) && (strcmp(rP->verb, "POST") == 0) && subs && (rP->endpoint == 1))
  {
    for (int i = 0; i < headers; i++)
    {
      if (strcasecmp(headerV[i].key, "Location") == 0)
        ownTrack(rP->connId, headerV[i].value, rP->tenant);
    }
  }
  else if ((status == 204) && (strcmp(rP->verb, "PATCH") == 0) && one && (rP->endpoint == 1))
    ownTrack(rP->connId, path, rP->tenant);          // changed to notify this connection
  else if ((status == 204) && (strcmp(rP->verb, "PATCH") == 0) && one && (rP->endpoint == 0))
    ownUntrack(rP->connId, path, rP->tenant);        // changed to notify elsewhere
  else if ((status == 204) && (strcmp(rP->verb, "DELETE") == 0) && (n > 0))
    ownUntrack(rP->connId, path, rP->tenant);

  reply(rP->connId, status, rP->requestId, headerV, headers, body, bodyLen);
}

static void runIgnore(int status, CorRestKeyValue* headerV, int headers, const char* body, int bodyLen, void* ctx)
{
  (void) status; (void) headerV; (void) headers; (void) body; (void) bodyLen; (void) ctx;
}



// -----------------------------------------------------------------------------
//
// hostOpened - a connection is up: its id, in the first message
//
static void hostClosed(int connId);

static void hostOpened(int connId)
{
  char hello[128];

  snprintf(hello, sizeof(hello), "{\"metadata\":{\"connection\":\"" CONN_PREFIX "%s-%d\"}}", bootId, connId);

  pthread_mutex_lock(&connMutex);
  Conn*            cP      = connFind(connId);
  TransportDriver* driverP = (cP != NULL) ? cP->driverP : NULL;
  pthread_mutex_unlock(&connMutex);

  if (driverP != NULL)
    driverP->send(connId, hello, strlen(hello));
}



// -----------------------------------------------------------------------------
//
// hostClosed - the connection is gone: the subscriptions that notify it, deleted (doc/websocket.md)
//
// A tracked subscription is read first: changed since over HTTP, or over another connection, to notify
// elsewhere, it is not this connection's any more, and stays.
//
typedef struct EndpointCtx
{
  const char*  connUri;                              // this connection's id
  bool         here;                                 // the subscription notifies it
} EndpointCtx;

static void endpointRespond(int status, CorRestKeyValue* headerV, int headers, const char* body, int bodyLen, void* ctx)
{
  EndpointCtx* eP = (EndpointCtx*) ctx;

  (void) headerV; (void) headers;

  if ((status != 200) || (body == NULL) || (bodyLen <= 0))
    return;

  CorAlloc ka;
  char     kaBuf[8 * 1024];
  CorJson  corJson;

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 64 * 1024, NULL, "transport endpoint");
  corJsonCreate(&corJson, &ka);

  char* copy = (char*) corAlloc(&ka, bodyLen + 1);

  if (copy != NULL)
  {
    memcpy(copy, body, bodyLen);
    copy[bodyLen] = 0;

    CorNode* treeP     = corJsonParse(&corJson, copy);
    CorNode* notifP    = ((treeP != NULL) && (treeP->type == CorObject)) ? corTreeLookup(treeP, "notification") : NULL;
    CorNode* endpointP = (notifP != NULL) ? corTreeLookup(notifP, "endpoint") : NULL;
    CorNode* uriP      = (endpointP != NULL) ? corTreeLookup(endpointP, "uri") : NULL;

    eP->here = (uriP != NULL) && (uriP->type == CorString) && (strcmp(uriP->value.s, eP->connUri) == 0);
  }

  corAllocBufferReset(&ka, false);
}

static void hostClosed(int connId)
{
  pthread_mutex_lock(&connMutex);

  Conn*  prevP = NULL;
  Conn*  cP    = connList;

  while ((cP != NULL) && (cP->connId != connId))
  {
    prevP = cP;
    cP    = cP->next;
  }

  if (cP != NULL)
  {
    if (prevP == NULL)
      connList = cP->next;
    else
      prevP->next = cP->next;
  }

  pthread_mutex_unlock(&connMutex);

  if (cP == NULL)
    return;

  char connUri[64];

  snprintf(connUri, sizeof(connUri), CONN_PREFIX "%s-%d", bootId, connId);

  for (int i = 0; i < cP->ownN; i++)
  {
    CorRestKeyValue tenantH = { (char*) "NGSILD-Tenant", cP->ownV[i].tenant };
    int             tenants = (cP->ownV[i].tenant != NULL) ? 1 : 0;
    EndpointCtx     e       = { connUri, false };

#if COR_FEATURE_AUTH
    authTrustNext();     // the broker's own requests: the connection the subscription notifies is gone
#endif
    corRestRunJson("GET", cP->ownV[i].path, &tenantH, tenants, NULL, 0, endpointRespond, &e);
#if COR_FEATURE_AUTH
    authTrustClear();
#endif

    if (e.here == true)
    {
#if COR_FEATURE_AUTH
      authTrustNext();
#endif
      corRestRunJson("DELETE", cP->ownV[i].path, &tenantH, tenants, NULL, 0, runIgnore, NULL);
#if COR_FEATURE_AUTH
      authTrustClear();
#endif
      COR_I("transport: connection %d closed - '%s' (tenant %s) deleted", connId, cP->ownV[i].path, (cP->ownV[i].tenant != NULL) ? cP->ownV[i].tenant : "default");
    }

    free(cP->ownV[i].path);
    free(cP->ownV[i].tenant);
  }

  free(cP->ownV);
  free(cP);
}



// -----------------------------------------------------------------------------
//
// hostMessage - a request: { "metadata": { "method", "path", "requestId", <headers> }, "body": ... }
//
// Any request the broker serves over HTTP (doc/websocket.md); a subscription's endpoint may name no
// connection but this one - by its id, or as urn:ngsi-ld:self.
//
static void hostMessage(int connId, const char* text, int len)
{
  CorAlloc ka;
  char     kaBuf[16 * 1024];
  CorJson  corJson;

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 64 * 1024, NULL, "transport message");
  corJsonCreate(&corJson, &ka);

  char* copy = (char*) corAlloc(&ka, len + 1);

  if (copy == NULL)
  {
    corAllocBufferReset(&ka, false);
    return;
  }

  memcpy(copy, text, len);
  copy[len] = 0;

  CorNode*    treeP     = corJsonParse(&corJson, copy);
  CorNode*    metaP     = (treeP != NULL) ? corTreeLookup(treeP, "metadata") : NULL;
  CorNode*    bodyP     = (treeP != NULL) ? corTreeLookup(treeP, "body") : NULL;
  CorNode*    methodP   = (metaP != NULL) ? corTreeLookup(metaP, "method") : NULL;
  CorNode*    pathP     = (metaP != NULL) ? corTreeLookup(metaP, "path") : NULL;
  CorNode*    reqIdP    = (metaP != NULL) ? corTreeLookup(metaP, "requestId") : NULL;
  const char* requestId = ((reqIdP != NULL) && (reqIdP->type == CorString)) ? reqIdP->value.s : NULL;

  if ((treeP == NULL) || (treeP->type != CorObject) || (metaP == NULL) || (metaP->type != CorObject))
  {
    replyError(connId, requestId, 400, "https://uri.etsi.org/ngsi-ld/errors/BadRequestData", "Bad Request",
               "a message is a JSON object with \"metadata\" (an object) and \"body\"");
    corAllocBufferReset(&ka, false);
    return;
  }

  if ((methodP == NULL) || (methodP->type != CorString) || (pathP == NULL) || (pathP->type != CorString))
  {
    replyError(connId, requestId, 400, "https://uri.etsi.org/ngsi-ld/errors/BadRequestData", "Bad Request",
               "a request names its \"method\" and \"path\" in \"metadata\"");
    corAllocBufferReset(&ka, false);
    return;
  }

  const char* path = pathP->value.s;

  //
  // The endpoint a subscription created or changed here names: none of another connection
  //
  CorNode* notifP    = ((bodyP != NULL) && (bodyP->type == CorObject)) ? corTreeLookup(bodyP, "notification") : NULL;
  CorNode* endpointP = (notifP != NULL) ? corTreeLookup(notifP, "endpoint") : NULL;
  CorNode* uriP      = (endpointP != NULL) ? corTreeLookup(endpointP, "uri") : NULL;

  //
  // urn:ngsi-ld:self - this connection (doc/sse.md § 2): an alias of its id, replaced by it here, so that
  // the request, the subscription it makes and its notifications are as with the id itself
  //
  if ((uriP != NULL) && (uriP->type == CorString) && (strcmp(uriP->value.s, "urn:ngsi-ld:self") == 0))
  {
    char  self[128];
    int   selfLen = snprintf(self, sizeof(self), CONN_PREFIX "%s-%d", bootId, connId);
    char* idP     = (char*) corAlloc(&ka, selfLen + 1);

    if (idP != NULL)
    {
      memcpy(idP, self, selfLen + 1);
      uriP->value.s = idP;
    }
  }

  int      endpoint  = ((uriP == NULL) || (uriP->type != CorString)) ? -1 : (connIdOf(uriP->value.s) == connId) ? 1 : 0;

  if ((uriP != NULL) && (uriP->type == CorString) && (strncmp(uriP->value.s, CONN_PREFIX, CONN_PREFIX_LEN) == 0) && (connIdOf(uriP->value.s) != connId))
  {
    char detail[256];

    snprintf(detail, sizeof(detail), "a subscription made over a WebSocket notifies its own connection, " CONN_PREFIX "%s-%d", bootId, connId);
    replyError(connId, requestId, 403, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported", "Forbidden", detail);
    corAllocBufferReset(&ka, false);
    return;
  }

  //
  // The headers: every other string member of metadata
  //
  CorRestKeyValue headerV[33];
  int             headers = 0;
  const char*     tenant  = NULL;

  for (CorNode* mP = metaP->value.head; (mP != NULL) && (headers < 32); mP = mP->next)
  {
    if ((mP->type != CorString) || (strcmp(mP->name, "method") == 0) || (strcmp(mP->name, "path") == 0) || (strcmp(mP->name, "requestId") == 0))
      continue;

    if (strcasecmp(mP->name, "NGSILD-Tenant") == 0)
      tenant = mP->value.s;

    headerV[headers].key   = mP->name;
    headerV[headers].value = mP->value.s;
    ++headers;
  }

  //
  // The tenant is the request's own NGSILD-Tenant, none the default tenant - as over HTTP. A connection
  // has no tenant (nor any other NGSI-LD header): a message means the same on every connection.
  //
  //
  // The body, rendered back to text: the request runs as an HTTP one, from its JSON
  //
  char* body    = NULL;
  int   bodyLen = 0;

  if (bodyP != NULL)
  {
    CorNode bodyNode = *bodyP;

    bodyNode.name = NULL;
    bodyNode.next = NULL;
    body          = (char*) corAlloc(&ka, corJsonFastRenderSize(&bodyNode) + 1);

    if (body != NULL)
    {
      corJsonFastRender(&bodyNode, body);
      bodyLen = strlen(body);
    }
  }

  RunCtx run = { connId, requestId, methodP->value.s, path, tenant, endpoint };

  //
  // The connection runs a request of its own, on this thread: what transportEndpointOwn asks, when the
  // request makes a subscription that names a connection
  //
  runMark(connId, true);
  corRestRunJson(methodP->value.s, path, headerV, headers, body, bodyLen, runRespond, &run);
  runMark(connId, false);
  corAllocBufferReset(&ka, false);
}



// -----------------------------------------------------------------------------
//
// The HTTP upgrade (corRestSetUpgradeHook): GET /ngsi-ld/v1/ws, Upgrade: websocket
//
static const char* headerLookup(const char* key)
{
  for (int i = 0; i < corRest.in.httpHeaderCount; i++)
  {
    if (strcasecmp(corRest.in.httpHeaderV[i].key, key) == 0)
      return corRest.in.httpHeaderV[i].value;
  }

  return NULL;
}

static void respHeader(const char* key, const char* value)
{
  corRestOutHeaderAdd(corAllocStrdup(&corRest.kalloc, key), corAllocStrdup(&corRest.kalloc, value));
}

static int connNext = 1;                             // under connMutex

typedef struct UpgradeCtx
{
  TransportDriver*  driverP;
} UpgradeCtx;

static void upgradeTake(int fd, const char* extra, int extraLen, CorRestUpgradeClose closeFn, void* closeArg, void* ctx)
{
  UpgradeCtx*      uP      = (UpgradeCtx*) ctx;
  TransportDriver* driverP = uP->driverP;
  Conn*            cP      = (Conn*) calloc(1, sizeof(Conn));

  if (cP == NULL)
  {
    free(uP);
    closeFn(closeArg);
    return;
  }

  free(uP);

  //
  // Registered BEFORE the plugin has it: its thread may greet it (opened) the moment take starts it
  //
  pthread_mutex_lock(&connMutex);
  cP->connId  = connNext++;
  cP->driverP = driverP;
  cP->next    = connList;
  connList    = cP;
  pthread_mutex_unlock(&connMutex);

  if (driverP->take(cP->connId, fd, extra, extraLen, closeFn, closeArg) == false)
    hostClosed(cP->connId);
}

static CorRestUpgradeTake upgradeHook(const char* protocol, void** ctxP)
{
  if (strcmp(corRest.in.urlPath, WS_PATH) != 0)
  {
    corRestProblem(404, "https://uri.etsi.org/ngsi-ld/errors/ResourceNotFound", "Not Found", "an upgrade is served on " WS_PATH " only");
    return NULL;
  }

  TransportDriver* driverP = NULL;

  for (int i = 0; i < transportCount; i++)
  {
    if ((transports[i].upgrade != NULL) && (strcasecmp(transports[i].upgrade, protocol) == 0))
      driverP = &transports[i];
  }

  if (driverP == NULL)
  {
    corRestProblem(400, "https://uri.etsi.org/ngsi-ld/errors/BadRequestData", "Bad Request", "no transport for the upgrade '%s'", protocol);
    return NULL;
  }

  int         status = 400;
  const char* detail = "refused";

  if (driverP->handshake(headerLookup, respHeader, &status, &detail) == false)
  {
    corRestProblem(status, "https://uri.etsi.org/ngsi-ld/errors/BadRequestData", "Bad Request", "%s", detail);
    return NULL;
  }

  //
  // NGSI-LD headers belong to each message's metadata, never to the connection: one in the upgrade
  // request is refused, not ignored - a client must not believe it chose a tenant or an @context
  // for the connection. (A browser cannot set them on an upgrade anyway.)
  //
  static const char* ngsildHeaderV[] = { "NGSILD-Tenant", "Link", "NGSILD-Snapshot", NULL };

  for (int i = 0; ngsildHeaderV[i] != NULL; i++)
  {
    if (headerLookup(ngsildHeaderV[i]) != NULL)
    {
      corRestProblem(400, "https://uri.etsi.org/ngsi-ld/errors/BadRequestData", "Bad Request",
                     "'%s' in the upgrade request - NGSI-LD headers go in each message's metadata, the connection has none", ngsildHeaderV[i]);
      return NULL;
    }
  }

  UpgradeCtx* uP = (UpgradeCtx*) calloc(1, sizeof(UpgradeCtx));

  if (uP == NULL)
  {
    corRestProblem(500, "https://uri.etsi.org/ngsi-ld/errors/InternalError", "Internal Error", "out of memory");
    return NULL;
  }

  uP->driverP = driverP;

  corRest.out.httpStatusCode = 101;
  *ctxP = uP;
  return upgradeTake;
}



// -----------------------------------------------------------------------------
//
// transportLoad -
//
int transportLoad(const char* commaList, char* errorBuf, int errorBufSize)
{
  if (commaList == NULL)
    return 0;

  char buf[1024];
  char* saveptr = NULL;

  strncpy(buf, commaList, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;

  for (char* token = strtok_r(buf, ",", &saveptr); token != NULL; token = strtok_r(NULL, ",", &saveptr))
  {
    while (*token == ' ')
      token++;

    if (*token == 0)
      continue;

    if (transportCount >= TRANSPORTS_MAX)
    {
      snprintf(errorBuf, errorBufSize, "too many transport plugins (max %d)", TRANSPORTS_MAX);
      return -1;
    }

    char path[512];
    char openErr[512];

    corPluginResolve(corPluginBaseDir(), "transport", NULL, token, path, sizeof(path));

    TransportRegisterFunc registerFunc = (TransportRegisterFunc) corPluginOpen(path, "transportRegister", openErr, sizeof(openErr));

    if (registerFunc == NULL)
    {
      snprintf(errorBuf, errorBufSize, "transport plugin '%s' (%s): %s", token, path, openErr);
      return -1;
    }

    TransportDriver* driverP = &transports[transportCount];

    memset(driverP, 0, sizeof(TransportDriver));
    driverP->abiVersion = TRANSPORT_ABI_VERSION;
    registerFunc(driverP);
    ++transportCount;

    COR_I("transport plugin '%s' loaded (%s, ABI %d)", (driverP->name != NULL) ? driverP->name : token, path, driverP->abiVersion);
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// transportInit -
//
bool transportInit(void)
{
  if (transportCount == 0)
    return true;

  uint8_t r[8];

  if (getrandom(r, sizeof(r), 0) != (ssize_t) sizeof(r))
  {
    uint64_t v = (uint64_t) time(NULL) ^ ((uint64_t) getpid() << 32);
    memcpy(r, &v, sizeof(r));
  }

  for (int i = 0; i < 8; i++)
    snprintf(&bootId[i * 2], 3, "%02x", r[i]);

  host.abiVersion = TRANSPORT_ABI_VERSION;
  host.opened     = hostOpened;
  host.message    = hostMessage;
  host.closed     = hostClosed;

  for (int i = 0; i < transportCount; i++)
  {
    char errorBuf[256] = "";

    if ((transports[i].init != NULL) && (transports[i].init(&host, errorBuf, sizeof(errorBuf)) == false))
    {
      COR_E("transport plugin '%s': %s", transports[i].name, errorBuf);
      return false;
    }
  }

  corRestSetUpgradeHook(upgradeHook);
  return true;
}



// -----------------------------------------------------------------------------
//
// transportNotifyHas -
//
bool transportNotifyHas(const char* uri)
{
  int connId = connIdOf(uri);

  if (connId < 0)
    return false;

  pthread_mutex_lock(&connMutex);
  bool open = (connFind(connId) != NULL);
  pthread_mutex_unlock(&connMutex);

  return open;
}



// -----------------------------------------------------------------------------
//
// transportUri - a URI that names a transport connection - of this run or not
//
bool transportUri(const char* uri)
{
  return (uri != NULL) && (strncmp(uri, CONN_PREFIX, CONN_PREFIX_LEN) == 0);
}



// -----------------------------------------------------------------------------
//
// transportEndpointOwn - the URI names the connection whose request runs on this thread: a subscription
// made over a connection may notify it - one made over HTTP, cor:// or another connection may not
//
bool transportEndpointOwn(const char* uri)
{
  int connId = connIdOf(uri);

  if (connId < 0)
    return false;

  pthread_mutex_lock(&connMutex);
  Conn* cP  = connFind(connId);
  bool  own = (cP != NULL) && cP->running && pthread_equal(cP->runThread, pthread_self());
  pthread_mutex_unlock(&connMutex);

  return own;
}



// -----------------------------------------------------------------------------
//
// transportNotifySend -
//
bool transportNotifySend(const char* uri, const char* payload)
{
  int connId = connIdOf(uri);

  pthread_mutex_lock(&connMutex);
  Conn*            cP      = (connId >= 0) ? connFind(connId) : NULL;
  TransportDriver* driverP = (cP != NULL) ? cP->driverP : NULL;
  pthread_mutex_unlock(&connMutex);

  return (driverP != NULL) && driverP->send(connId, payload, strlen(payload));
}



// -----------------------------------------------------------------------------
//
// Leftover - a subscription of another run's connection, to delete
//
typedef struct Leftover
{
  char*             path;                            // /ngsi-ld/v1/subscriptions/<id> or /ngsi-ld/v1/csourceSubscriptions/<id>
  char*             tenant;                          // NULL: the default tenant
  struct Leftover*  next;
} Leftover;

static Leftover* leftoverAdd(Leftover* list, const char* base, const char* subId, const char* tenant)
{
  Leftover* lP = (Leftover*) calloc(1, sizeof(Leftover));

  if (lP == NULL)
    return list;

  int size = strlen(base) + 1 + strlen(subId) + 1;

  lP->path   = (char*) malloc(size);
  lP->tenant = ((tenant != NULL) && (tenant[0] != 0)) ? strdup(tenant) : NULL;
  lP->next   = list;

  if (lP->path == NULL)
  {
    free(lP->tenant);
    free(lP);
    return list;
  }

  snprintf(lP->path, size, "%s/%s", base, subId);
  return lP;
}

static bool leftoverUri(const char* uri)
{
  return (transportUri(uri) == true) && (connIdOf(uri) < 0);
}



// -----------------------------------------------------------------------------
//
// transportLeftoversDelete - the subscriptions of a run that is gone, deleted
//
// A broker killed, or crashed, deletes nothing: with a persistent store its connections' subscriptions
// are there at the next start, matched on every write, notifying no one. Every subscription and context
// source subscription whose endpoint is the connection of another run is deleted, in every tenant. The
// caches are walked, not the store: they hold every subscription, loaded at start.
//
// Not with --high-availability: another instance's run is not this one's either, and it may be alive.
//
void transportLeftoversDelete(void)
{
  Leftover* list = NULL;

  for (Tenant* tP = &tenant0; tP != NULL; tP = (tP == &tenant0) ? __atomic_load_n(&tenantList, __ATOMIC_ACQUIRE) : tP->next)
  {
    const char*     tenant = tenantApiName(tP);
    LdSubCache*     subsP  = (LdSubCache*) tP->subCacheP;
    LdSubCache*     csubsP = (LdSubCache*) tP->regSubCacheP;
    LdPernotCache*  pernotP = (LdPernotCache*) tP->pernotCacheP;

    if (subsP != NULL)
    {
      ldSubCacheRdLock(subsP);
      for (LdSubCacheItem* iP = subsP->itemList; iP != NULL; iP = iP->next)
      {
        if (leftoverUri(iP->endpointUri) == true)
          list = leftoverAdd(list, SUBS_PATH, iP->subId, tenant);
      }
      ldSubCacheUnlock(subsP);
    }

    if (pernotP != NULL)
    {
      ldPernotCacheRdLock(pernotP);
      for (LdPernotItem* iP = pernotP->head; iP != NULL; iP = iP->next)
      {
        if (leftoverUri(iP->endpointUri) == true)
          list = leftoverAdd(list, SUBS_PATH, iP->subId, tenant);
      }
      ldPernotCacheUnlock(pernotP);
    }

    if (csubsP != NULL)
    {
      ldSubCacheRdLock(csubsP);
      for (LdSubCacheItem* iP = csubsP->itemList; iP != NULL; iP = iP->next)
      {
        if (leftoverUri(iP->endpointUri) == true)
          list = leftoverAdd(list, CSUBS_PATH, iP->subId, tenant);
      }
      ldSubCacheUnlock(csubsP);
    }
  }

  while (list != NULL)
  {
    Leftover*       lP      = list;
    CorRestKeyValue tenantH = { (char*) "NGSILD-Tenant", lP->tenant };

#if COR_FEATURE_AUTH
    authTrustNext();     // the broker's own request: the connection the subscription notified is of a run that is gone
#endif
    corRestRunJson("DELETE", lP->path, &tenantH, (lP->tenant != NULL) ? 1 : 0, NULL, 0, runIgnore, NULL);
#if COR_FEATURE_AUTH
    authTrustClear();
#endif
    COR_I("transport: '%s' (tenant %s) deleted - it notified a connection of another run", lP->path, (lP->tenant != NULL) ? lP->tenant : "default");

    list = lP->next;
    free(lP->path);
    free(lP->tenant);
    free(lP);
  }
}



// -----------------------------------------------------------------------------
//
// transportStop -
//
void transportStop(void)
{
  for (int i = 0; i < transportCount; i++)
  {
    if (transports[i].stop != NULL)
      transports[i].stop();
  }
}

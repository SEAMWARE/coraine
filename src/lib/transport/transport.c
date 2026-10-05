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
#define SUBS_PATH_LEN    25
#define ENTITIES_PATH    "/ngsi-ld/v1/entities"



// -----------------------------------------------------------------------------
//
// pathUnder - is 'path' the resource 'prefix', or under it (a '/' after it), or it with a query?
//
static bool pathUnder(const char* path, const char* prefix)
{
  size_t n = strlen(prefix);

  return (strncmp(path, prefix, n) == 0) && ((path[n] == 0) || (path[n] == '/') || (path[n] == '?'));
}



// -----------------------------------------------------------------------------
//
// Conn - an open connection, and the subscriptions it created (deleted when it closes)
//
typedef struct Conn
{
  int               connId;
  TransportDriver*  driverP;
  char**            subV;
  int               subN;
  int               subSize;
  struct Conn*      next;
} Conn;

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
// connIdOf - the connection a URI names (urn:ngsi-ld:WebSocket:<n>), -1 if none
//
static int connIdOf(const char* uri)
{
  if ((uri == NULL) || (strncmp(uri, CONN_PREFIX, CONN_PREFIX_LEN) != 0))
    return -1;

  char* end = NULL;
  long  n   = strtol(&uri[CONN_PREFIX_LEN], &end, 10);

  return ((end != NULL) && (*end == 0) && (n >= 0)) ? (int) n : -1;
}



// -----------------------------------------------------------------------------
//
// subTrack / subUntrack - a subscription this connection created, or deleted
//
static void subTrack(int connId, const char* subId)
{
  pthread_mutex_lock(&connMutex);

  Conn* cP = connFind(connId);

  if (cP != NULL)
  {
    if (cP->subN == cP->subSize)
    {
      int    size = (cP->subSize == 0) ? 8 : cP->subSize * 2;
      char** v    = (char**) realloc(cP->subV, size * sizeof(char*));

      if (v != NULL)
      {
        cP->subV    = v;
        cP->subSize = size;
      }
    }

    if (cP->subN < cP->subSize)
      cP->subV[cP->subN++] = strdup(subId);
  }

  pthread_mutex_unlock(&connMutex);
}

static void subUntrack(int connId, const char* subId)
{
  pthread_mutex_lock(&connMutex);

  Conn* cP = connFind(connId);

  for (int i = 0; (cP != NULL) && (i < cP->subN); i++)
  {
    if (strcmp(cP->subV[i], subId) == 0)
    {
      free(cP->subV[i]);
      cP->subV[i] = cP->subV[--cP->subN];
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
    bufAdd(&b, ",\"body\":", 8);
    bufAdd(&b, body, bodyLen);
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
} RunCtx;

static void runRespond(int status, CorRestKeyValue* headerV, int headers, const char* body, int bodyLen, void* ctx)
{
  RunCtx* rP = (RunCtx*) ctx;

  //
  // The subscriptions this connection creates and deletes - only those: a 201's Location is an
  // entity's, a registration's ... on other paths
  //
  if ((status == 201) && (strcmp(rP->verb, "POST") == 0) && (strcmp(rP->path, SUBS_PATH) == 0))
  {
    for (int i = 0; i < headers; i++)
    {
      if (strcasecmp(headerV[i].key, "Location") == 0)
      {
        const char* slash = strrchr(headerV[i].value, '/');

        if (slash != NULL)
          subTrack(rP->connId, &slash[1]);
      }
    }
  }
  else if ((status == 204) && (strcmp(rP->verb, "DELETE") == 0) && (strncmp(rP->path, SUBS_PATH "/", SUBS_PATH_LEN + 1) == 0))
    subUntrack(rP->connId, &rP->path[SUBS_PATH_LEN + 1]);

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

  snprintf(hello, sizeof(hello), "{\"metadata\":{\"connection\":\"" CONN_PREFIX "%d\"}}", connId);

  pthread_mutex_lock(&connMutex);
  Conn*            cP      = connFind(connId);
  TransportDriver* driverP = (cP != NULL) ? cP->driverP : NULL;
  pthread_mutex_unlock(&connMutex);

  if (driverP != NULL)
    driverP->send(connId, hello, strlen(hello));
}



// -----------------------------------------------------------------------------
//
// hostClosed - the connection is gone: the subscriptions it created, deleted (doc/websocket.md § 6)
//
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

  for (int i = 0; i < cP->subN; i++)
  {
    char path[512];

    snprintf(path, sizeof(path), SUBS_PATH "/%s", cP->subV[i]);
    corRestRunJson("DELETE", path, NULL, 0, NULL, 0, runIgnore, NULL);
    COR_I("transport: connection %d closed - its subscription '%s' deleted", connId, cP->subV[i]);
    free(cP->subV[i]);
  }

  free(cP->subV);
  free(cP);
}



// -----------------------------------------------------------------------------
//
// hostMessage - a request: { "metadata": { "method", "path", "requestId", <headers> }, "body": ... }
//
// Served: /ngsi-ld/v1/subscriptions and /ngsi-ld/v1/entities (doc/websocket.md), and a subscription's
// endpoint may name no connection but this one.
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

  if ((pathUnder(path, SUBS_PATH) == false) && (pathUnder(path, ENTITIES_PATH) == false))
  {
    replyError(connId, requestId, 501, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported", "Not Implemented",
               "over a WebSocket: /ngsi-ld/v1/subscriptions and /ngsi-ld/v1/entities");
    corAllocBufferReset(&ka, false);
    return;
  }

  //
  // The endpoint a subscription created or changed here names: none of another connection
  //
  CorNode* notifP    = ((bodyP != NULL) && (bodyP->type == CorObject)) ? corTreeLookup(bodyP, "notification") : NULL;
  CorNode* endpointP = (notifP != NULL) ? corTreeLookup(notifP, "endpoint") : NULL;
  CorNode* uriP      = (endpointP != NULL) ? corTreeLookup(endpointP, "uri") : NULL;

  if ((uriP != NULL) && (uriP->type == CorString) && (strncmp(uriP->value.s, CONN_PREFIX, CONN_PREFIX_LEN) == 0) && (connIdOf(uriP->value.s) != connId))
  {
    char detail[256];

    snprintf(detail, sizeof(detail), "a subscription made over a WebSocket notifies its own connection, " CONN_PREFIX "%d", connId);
    replyError(connId, requestId, 403, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported", "Forbidden", detail);
    corAllocBufferReset(&ka, false);
    return;
  }

  //
  // The headers: every other string member of metadata
  //
  CorRestKeyValue headerV[32];
  int             headers = 0;

  for (CorNode* mP = metaP->value.head; (mP != NULL) && (headers < 32); mP = mP->next)
  {
    if ((mP->type != CorString) || (strcmp(mP->name, "method") == 0) || (strcmp(mP->name, "path") == 0) || (strcmp(mP->name, "requestId") == 0))
      continue;

    headerV[headers].key   = mP->name;
    headerV[headers].value = mP->value.s;
    ++headers;
  }

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

  RunCtx run = { connId, requestId, methodP->value.s, path };

  corRestRunJson(methodP->value.s, path, headerV, headers, body, bodyLen, runRespond, &run);
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

static void upgradeTake(int fd, const char* extra, int extraLen, CorRestUpgradeClose closeFn, void* closeArg, void* ctx)
{
  TransportDriver* driverP = (TransportDriver*) ctx;
  Conn*            cP      = (Conn*) calloc(1, sizeof(Conn));

  if (cP == NULL)
  {
    closeFn(closeArg);
    return;
  }

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

  corRest.out.httpStatusCode = 101;
  *ctxP = driverP;
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

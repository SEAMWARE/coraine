//
// FILE            bridgeHttpListen.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#define _GNU_SOURCE                                   // pthread_timedjoin_np
#include <pthread.h>                                  // pthread_*, pthread_timedjoin_np
#include <stdbool.h>                                  // bool
#include <stdlib.h>                                   // free
#include <string.h>                                   // strcmp, strdup, memcpy, memset
#include <time.h>                                     // clock_gettime

#include "corLog/corLog.h"                            // COR_W, COR_V
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corHttp/CorHttp.h"                          // CorHttpServer, CorHttpConn, corHttp*
#include "corRest/corRestUrlValueEncode.h"            // corRestUrlValueDecode
#include "corBridge/BridgeBroker.h"                   // BridgeHttpRequest, BridgeHttpHandler, BRIDGE_*

#include "bridge/bridgeHttpListen.h"                  // Own interface



//
// LISTENERS_MAX - ports, all bridges together; a bridge has one or two
//
#define LISTENERS_MAX   8
//
// BODY_MAX - the largest body a port reads: an IoT Agent's (Express's) default, 1 MB
//
#define BODY_MAX        (1024 * 1024)



// -----------------------------------------------------------------------------
//
// Listener - one port: its server, the thread that runs its loop, and whose it is
//
typedef struct Listener
{
  char*              bridgeName;                      // NULL: a free slot
  unsigned short     port;
  BridgeHttpHandler  handler;
  CorHttpServer      server;
  pthread_t          thread;
} Listener;

static Listener         listenerV[LISTENERS_MAX];
static pthread_mutex_t  listenerMutex = PTHREAD_MUTEX_INITIALIZER;



// -----------------------------------------------------------------------------
//
// textOf - a slice as a string of the request's allocator, to be decoded in place
//
static char* textOf(CorHttpConn* connP, const CorHttpSlice* sliceP)
{
  char* s = corAlloc(&connP->alloc, sliceP->len + 1);

  if (s == NULL)
    return NULL;

  memcpy(s, sliceP->s, sliceP->len);
  s[sliceP->len] = 0;
  return s;
}



// -----------------------------------------------------------------------------
//
// formDecode - a query's key or value: '+' a space, then %XX - so %2B stays a '+'
//
static void formDecode(char* s)
{
  for (char* p = s; *p != 0; p++)
  {
    if (*p == '+')
      *p = ' ';
  }

  corRestUrlValueDecode(s);
}



// -----------------------------------------------------------------------------
//
// requestParam / requestHeader - BridgeHttpRequest's accessors
//
static const char* requestParam(const BridgeHttpRequest* reqP, const char* key)
{
  CorHttpConn* connP = reqP->opaque;

  for (int ix = 0; ix < connP->uriParams; ix++)
  {
    char* k = textOf(connP, &connP->uriParam[ix].key);

    if (k == NULL)
      return NULL;

    formDecode(k);
    if (strcmp(k, key) != 0)
      continue;

    char* v = textOf(connP, &connP->uriParam[ix].value);

    if (v != NULL)
      formDecode(v);

    return v;
  }

  return NULL;
}

static const char* requestHeader(const BridgeHttpRequest* reqP, const char* key)
{
  return corHttpHeader((CorHttpConn*) reqP->opaque, key);
}



// -----------------------------------------------------------------------------
//
// requestServe - corHttp's callback: the request to the handler of the port it came in on
//
static void requestServe(CorHttpConn* connP)
{
  Listener* lP = NULL;

  for (int ix = 0; ix < LISTENERS_MAX; ix++)
  {
    if (&listenerV[ix].server == connP->serverP)
      lP = &listenerV[ix];
  }

  char* method = textOf(connP, &connP->method);
  char* path   = textOf(connP, &connP->path);

  if ((lP == NULL) || (method == NULL) || (path == NULL))
  {
    corHttpResponseStatus(connP, 500);
    return;
  }

  corRestUrlValueDecode(path);

  BridgeHttpRequest req;

  req.method     = method;
  req.path       = path;
  req.body       = ((connP->body.s != NULL) && (connP->bodyRefused == false)) ? connP->body.s : "";
  req.bodyLen    = (req.body[0] != 0) ? connP->body.len : 0;
  req.bodyTooBig = (connP->bodyRefused == true) ? 1 : 0;
  req.param      = requestParam;
  req.header     = requestHeader;
  req.opaque     = connP;

  char*       body        = NULL;
  const char* contentType = NULL;
  int         status      = lP->handler(&req, &body, &contentType);

  if ((status < 100) || (status > 599))
  {
    COR_W("bridge %s: its handler answered %d for %s %s - a 500", lP->bridgeName, status, method, path);
    status = 500;
  }

  corHttpResponseStatus(connP, status);

  if (body != NULL)
  {
    int   len  = strlen(body);
    char* copy = corAlloc(&connP->alloc, len + 1);   // the response is written after this returns

    if (copy != NULL)
    {
      memcpy(copy, body, len + 1);
      corHttpResponseHeader(connP, "Content-Type", (contentType != NULL) ? contentType : "application/json");
      corHttpResponseBody(connP, copy, len);
    }
    else
      corHttpResponseStatus(connP, 500);

    free(body);
  }
}



// -----------------------------------------------------------------------------
//
// listenerRun - the port's thread: its loop, until bridgeHttpListenStop
//
static void* listenerRun(void* arg)
{
  Listener* lP = arg;

  corHttpServe(&lP->server);
  corHttpServeEnd(&lP->server);
  return NULL;
}



// -----------------------------------------------------------------------------
//
// bridgeHttpListen -
//
int bridgeHttpListen(const char* bridgeName, unsigned short port, BridgeHttpHandler handler)
{
  if ((bridgeName == NULL) || (port == 0) || (handler == NULL))
    return BRIDGE_BAD_INPUT;

  pthread_mutex_lock(&listenerMutex);

  Listener* lP = NULL;

  for (int ix = 0; (ix < LISTENERS_MAX) && (lP == NULL); ix++)
  {
    if (listenerV[ix].bridgeName == NULL)
      lP = &listenerV[ix];
  }

  if (lP == NULL)
  {
    pthread_mutex_unlock(&listenerMutex);
    COR_W("bridge %s: port %u not served - the broker serves %d bridge ports at most", bridgeName, port, LISTENERS_MAX);
    return BRIDGE_ERR;
  }

  memset(lP, 0, sizeof(Listener));

  if (corHttpInit(&lP->server, port, 64, requestServe) != CorHttpOk)
  {
    pthread_mutex_unlock(&listenerMutex);
    COR_W("bridge %s: cannot listen on port %u", bridgeName, port);
    return BRIDGE_ERR;
  }

  lP->server.maxRequestSize = BODY_MAX + 16 * 1024;  // the body, and its request line and headers
  lP->bridgeName            = strdup(bridgeName);
  lP->port                  = port;
  lP->handler               = handler;

  if ((lP->bridgeName == NULL) || (pthread_create(&lP->thread, NULL, listenerRun, lP) != 0))
  {
    corHttpRelease(&lP->server);
    free(lP->bridgeName);
    lP->bridgeName = NULL;
    pthread_mutex_unlock(&listenerMutex);
    COR_W("bridge %s: port %u not served - no thread for it", bridgeName, port);
    return BRIDGE_ERR;
  }

  pthread_mutex_unlock(&listenerMutex);
  COR_V("bridge %s: serving port %u", bridgeName, port);
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// bridgeHttpListenStop -
//
void bridgeHttpListenStop(const char* bridgeName)
{
  pthread_mutex_lock(&listenerMutex);

  for (int ix = 0; ix < LISTENERS_MAX; ix++)
  {
    Listener* lP = &listenerV[ix];

    if ((lP->bridgeName == NULL) || (strcmp(lP->bridgeName, bridgeName) != 0))
      continue;

    //
    // corHttpServe sets its running flag as it starts: a stop before that is lost, so it is said again
    // until the loop has ended - and then no handler runs
    //
    for (;;)
    {
      struct timespec deadline;

      corHttpStop(&lP->server);
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_nsec += 100 * 1000 * 1000;
      if (deadline.tv_nsec >= 1000000000)
      {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000000000;
      }

      if (pthread_timedjoin_np(lP->thread, NULL, &deadline) == 0)
        break;
    }

    corHttpRelease(&lP->server);
    free(lP->bridgeName);
    lP->bridgeName = NULL;
  }

  pthread_mutex_unlock(&listenerMutex);
}

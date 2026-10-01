//
// FILE            corRequest.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corRequest - a cor:// client for tests and benchmarks: what curl and wrk are for HTTP
//
//   one request, like curl -i:
//     corRequest --url cor://localhost:2026 --path /ngsi-ld/v1/entities/urn:E1
//     corRequest --url cor://localhost:2026 --verb POST --path /ngsi-ld/v1/entities --payload '{...}'
//     prints "STATUS <code>", the headers, an empty line and the body as JSON; exit 0 when a
//     response came back (whatever its status), 1 when none did
//
//   load, like wrk:
//     corRequest --url cor://localhost:2026 --path ... -c 16 --duration 8
//     <c> connections, each a thread of its own sending one request after the other for <d>
//     seconds; prints requests/second and the p50 / p90 / p99 / max latency
//
// Headers: --header 'Name: value', several separated by '|'.
//
//   as the functests' corCurl, over cor:// instead of curl:
//     corRequest --url ... --path ... --curl --bodyFile /tmp/x.body --pretty 2
//     the header block exactly as curl -D shows the HTTP server's - the status line with its reason
//     phrase, Date, the broker's headers, Content-Length - and the body, as the broker sends it, in
//     bodyFile. corCurl then treats that file as it treats curl's. Content-Length is the size the
//     broker WOULD have rendered: its JSON at --pretty spaces per indent (test brokers run with
//     --pretty-print 2).
//
//   exit: 0 a response; 1 no response; 3 the payload is not JSON - a request only HTTP can carry
//
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // printf, fprintf
#include <stdlib.h>                                   // malloc, free, qsort
#include <string.h>                                   // strchr, strlen, strtok_r
#include <time.h>                                     // clock_gettime
#include <pthread.h>                                  // pthread_create, pthread_join

#include "corArgs/corArgs.h"                          // corArgsInit, corArgsParse
#include "corArgs/corArgsBuiltins.h"                  // corArgsBuiltinVerbose
#include "corLog/corLog.h"                            // corLogInit
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corTree/CorNode.h"                          // CorNode
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonRender.h"                    // corJsonRender
#include "corJson/corJsonRenderSize.h"                // corJsonRenderSize, corJsonFastRenderSize
#include "corRest/CorRestVerb.h"                      // CorRestVerb, corRestVerbFromString
#include "corRest/CorRestKeyValue.h"                  // CorRestKeyValue
#include "corRest/corRestCor.h"                       // corRestCorInit, corRestCorSend
#include "corNgsild/ldBinCodec.h"                     // ldBinCodec, ldBinNamespaceV
#include "corNgsild/CorTerm.h"                        // CorTermLast



// -----------------------------------------------------------------------------
//
// Options
//
static char*         url          = NULL;
static char*         path         = (char*) "/ngsi-ld/v1/entities";
static char*         verb         = (char*) "GET";
static char*         payload      = NULL;
static char*         headers      = NULL;
static unsigned int  connections  = 0;
static unsigned int  seconds      = 8;
static unsigned int  timeoutMs    = 30000;
static bool          curlMode     = false;
static char*         bodyFile     = NULL;
static int           pretty       = 0;

static CorArg argV[] =
{
  { "--url",     NULL, CorArgString, _vp &url,         CorArgReq, NULL,              NULL,   NULL,          "cor://host:port" },
  { "--path",    NULL, CorArgString, _vp &path,        CorArgOpt, _vp "/ngsi-ld/v1/entities", NULL, NULL, "path and query" },
  { "--verb",    "-X", CorArgString, _vp &verb,        CorArgOpt, _vp "GET",         NULL,   NULL,          "GET, POST, PATCH, PUT, DELETE" },
  { "--payload", NULL, CorArgString, _vp &payload,     CorArgOpt, NULL,              NULL,   NULL,          "request body (JSON)" },
  { "--header",  "-H", CorArgString, _vp &headers,     CorArgOpt, NULL,              NULL,   NULL,          "'Name: value', several separated by '|'" },
  { "--conns",   "-c", CorArgUInt,   _vp &connections, CorArgOpt, _vp 0,             _vp 0,  _vp 1024,      "load mode: connections (0: one request)" },
  { "--duration",NULL, CorArgUInt,   _vp &seconds,     CorArgOpt, _vp 8,             _vp 1,  _vp 3600,      "load mode: seconds" },
  { "--timeout", NULL, CorArgUInt,   _vp &timeoutMs,   CorArgOpt, _vp 30000,         _vp 1,  _vp 600000,    "milliseconds" },
  { "--curl",    NULL, CorArgBool,   _vp &curlMode,    CorArgOpt, _vp false,         _vp false, _vp true,   "print as the functests' corCurl does (with --bodyFile)" },
  { "--bodyFile",NULL, CorArgString, _vp &bodyFile,    CorArgOpt, NULL,              NULL,   NULL,          "--curl: where the body goes" },
  { "--pretty",  NULL, CorArgInt,    _vp &pretty,      CorArgOpt, _vp 0,             _vp 0,  _vp 16,        "--curl: the broker's --pretty-print, for Content-Length" },
  CORARGS_END
};



// -----------------------------------------------------------------------------
//
// headerV - the --header option, split
//
static CorRestKeyValue headerV[32];
static int             headerCount = 0;

static void headersSplit(void)
{
  if (headers == NULL)
    return;

  char* save = NULL;

  for (char* h = strtok_r(headers, "|", &save); (h != NULL) && (headerCount < 32); h = strtok_r(NULL, "|", &save))
  {
    char* colon = strchr(h, ':');

    if (colon == NULL)
      continue;

    *colon = 0;
    char* value = &colon[1];
    while (*value == ' ')
      ++value;

    headerV[headerCount].key   = h;
    headerV[headerCount].value = value;
    headerCount += 1;
  }
}



// -----------------------------------------------------------------------------
//
// once - one request, printed like curl -i
//
static int once(CorRestVerb v)
{
  CorAlloc           ka;
  static char        kaBuf[64 * 1024];
  CorRestCorResponse resp;
  const char*        error = NULL;

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 64 * 1024, NULL, "corRequest");

  if (corRestCorSend(url, v, path, headerV, headerCount, NULL, payload, timeoutMs, &ka, &resp, &error) == false)
  {
    fprintf(stderr, "corRequest: %s\n", (error != NULL) ? error : "failed");
    return 1;
  }

  printf("STATUS %d\n", resp.status);
  for (int i = 0; i < resp.headerCount; i++)
    printf("%s: %s\n", resp.headerV[i].key, resp.headerV[i].value);
  printf("\n");

  if (resp.bodyTree != NULL)
  {
    CorJson cj;

    corJsonCreate(&cj, &ka);
    char* out = corAlloc(&ka, corJsonRenderSize(&cj, resp.bodyTree) + 1);
    corJsonRender(&cj, resp.bodyTree, out);
    printf("%s\n", out);
  }
  else if (resp.bodyText != NULL)
    printf("%s\n", resp.bodyText);

  corAllocBufferReset(&ka, false);
  return 0;
}



// -----------------------------------------------------------------------------
//
// reasonPhrase - the HTTP server's own (libmicrohttpd's) - the functests compare it
//
static const char* reasonPhrase(int status)
{
  switch (status)
  {
  case 100: return "Continue";
  case 200: return "OK";
  case 201: return "Created";
  case 202: return "Accepted";
  case 204: return "No Content";
  case 207: return "Multi-Status";
  case 301: return "Moved Permanently";
  case 302: return "Found";
  case 304: return "Not Modified";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 406: return "Not Acceptable";
  case 409: return "Conflict";
  case 411: return "Length Required";
  case 412: return "Precondition Failed";
  case 413: return "Content Too Large";
  case 415: return "Unsupported Media Type";
  case 422: return "Unprocessable Content";
  case 429: return "Too Many Requests";
  case 500: return "Internal Server Error";
  case 501: return "Not Implemented";
  case 502: return "Bad Gateway";
  case 503: return "Service Unavailable";
  case 504: return "Gateway Timeout";
  case 508: return "Loop Detected";
  default:  return "Unknown";
  }
}



// -----------------------------------------------------------------------------
//
// curlOnce - one request, shown as corCurl shows an HTTP one
//
// The HTTP server writes Date first and Content-Length last, around the broker's own headers - which
// cor:// carries in the same order. A 204 has no Content-Length.
//
static int curlOnce(CorRestVerb v)
{
  CorAlloc           ka;
  static char        kaBuf[64 * 1024];
  CorRestCorResponse resp;
  const char*        error = NULL;

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 256 * 1024, NULL, "corRequest");

  if (corRestCorSend(url, v, path, headerV, headerCount, NULL, payload, timeoutMs, &ka, &resp, &error) == false)
  {
    if ((error != NULL) && (strcmp(error, "the request body is not JSON") == 0))
      return 3;

    fprintf(stderr, "corRequest: %s\n", (error != NULL) ? error : "failed");
    return 1;
  }

  //
  // The body, as the broker would have rendered it for HTTP
  //
  char* body    = NULL;
  int   bodyLen = 0;

  if (resp.bodyTree != NULL)
  {
    CorJson cj;

    corJsonCreate(&cj, &ka);
    if (pretty > 0)
    {
      cj.spacesPerIndent = pretty;
      body = corAlloc(&ka, corJsonRenderSize(&cj, resp.bodyTree) + 1);
      corJsonRender(&cj, resp.bodyTree, body);
    }
    else
    {
      body = corAlloc(&ka, corJsonFastRenderSize(resp.bodyTree) + 1);
      corJsonFastRender(resp.bodyTree, body);
    }
    bodyLen = strlen(body);
  }
  else if (resp.bodyText != NULL)
  {
    body    = resp.bodyText;
    bodyLen = strlen(body);
  }

  char      date[64];
  time_t    now = time(NULL);
  struct tm tm;

  gmtime_r(&now, &tm);
  strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);

  printf("HTTP/1.1 %d %s\n", resp.status, reasonPhrase(resp.status));
  printf("Date: %s\n", date);
  for (int i = 0; i < resp.headerCount; i++)
    printf("%s: %s\n", resp.headerV[i].key, resp.headerV[i].value);
  if (resp.status != 204)
    printf("Content-Length: %d\n", bodyLen);

  if (bodyFile != NULL)
  {
    FILE* fP = fopen(bodyFile, "w");

    if (fP != NULL)
    {
      if (bodyLen > 0)
        fwrite(body, 1, bodyLen, fP);
      fclose(fP);
    }
  }

  corAllocBufferReset(&ka, false);
  return 0;
}


// -----------------------------------------------------------------------------
//
// Load mode
//
typedef struct Worker
{
  pthread_t  tid;
  CorRestVerb verb;
  double     endAt;
  long       done;
  long       errors;
  double*    latV;        // microseconds
  long       latMax;
} Worker;

static double now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void* worker(void* arg)
{
  Worker*            wP = (Worker*) arg;
  CorAlloc           ka;
  char*              kaBuf = malloc(64 * 1024);
  CorRestCorResponse resp;
  const char*        error;

  corAllocBufferInit(&ka, kaBuf, 64 * 1024, 64 * 1024, NULL, "corRequest load");

  for (double t0 = now(); t0 < wP->endAt; t0 = now())
  {
    bool ok = corRestCorSend(url, wP->verb, path, headerV, headerCount, NULL, payload, timeoutMs, &ka, &resp, &error);

    if ((ok == true) && (resp.status < 400))
    {
      if (wP->done < wP->latMax)
        wP->latV[wP->done] = (now() - t0) * 1e6;
      wP->done += 1;
    }
    else
      wP->errors += 1;

    corAllocBufferReset(&ka, true);                  // reuse - a reset in a loop must not free the buffer
  }

  corAllocBufferReset(&ka, false);
  free(kaBuf);
  return NULL;
}

static int doubleCompare(const void* a, const void* b)
{
  double x = *(const double*) a;
  double y = *(const double*) b;

  return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static int load(CorRestVerb v)
{
  Worker* wV     = calloc(connections, sizeof(Worker));
  long    latMax = 4000000 / connections + 1000;
  double  start  = now();

  for (unsigned int i = 0; i < connections; i++)
  {
    wV[i].verb   = v;
    wV[i].endAt  = start + seconds;
    wV[i].latMax = latMax;
    wV[i].latV   = malloc(latMax * sizeof(double));
    pthread_create(&wV[i].tid, NULL, worker, &wV[i]);
  }

  long done = 0, errors = 0, kept = 0;

  for (unsigned int i = 0; i < connections; i++)
  {
    pthread_join(wV[i].tid, NULL);
    done   += wV[i].done;
    errors += wV[i].errors;
    kept   += (wV[i].done < latMax) ? wV[i].done : latMax;
  }

  double  elapsed = now() - start;
  double* allV    = malloc((kept + 1) * sizeof(double));
  long    n       = 0;

  for (unsigned int i = 0; i < connections; i++)
  {
    long k = (wV[i].done < latMax) ? wV[i].done : latMax;

    memcpy(&allV[n], wV[i].latV, k * sizeof(double));
    n += k;
    free(wV[i].latV);
  }

  qsort(allV, n, sizeof(double), doubleCompare);

  #define PCT(p) ((n > 0) ? allV[(long) ((n - 1) * (p))] : 0.0)
  printf("%u connections, %u s: %ld requests, %ld errors, %.2f req/s  |  p50 %.0f us  p90 %.0f us  p99 %.0f us  max %.0f us\n",
         connections, seconds, done, errors, done / elapsed, PCT(0.50), PCT(0.90), PCT(0.99), (n > 0) ? allV[n - 1] : 0.0);

  free(allV);
  free(wV);
  return (errors == 0) ? 0 : 1;
}



// -----------------------------------------------------------------------------
//
// main -
//
int main(int argC, char* argV_[])
{
  if ((corArgsInit("corRequest", argV, "CORREQUEST") != CorArgsOk) || (corArgsParse(argC, argV_) != CorArgsOk))
    return 2;

  if (corLogInit("corRequest", "/tmp", false, NULL, "0-255", corArgsBuiltinVerbose, corArgsBuiltinDebug, false) != 0)
    return 2;

  headersSplit();
  corRestCorInit(&ldBinCodec, ldBinNamespaceV, ldBinNamespaces, CorTermLast);

  CorRestVerb v = corRestVerbFromString(verb);

  if (connections > 0)
    return load(v);

  return (curlMode == true) ? curlOnce(v) : once(v);
}

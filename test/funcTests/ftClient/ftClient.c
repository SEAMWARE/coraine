//
// FILE            ftClient.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Test notification receiver for functional tests.
// Accumulates all incoming requests (e.g. POST /notify) in a global array.
// Provides:
//   GET    /dump   - return all accumulated requests as text
//   DELETE /dump   - clear the accumulator
//   GET    /die    - exit the process
//   POST   /**     - accumulate the request (catch-all)
//
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

#include <mosquitto.h>

#include "kargs/kargs.h"
#include "kargs/kargsBuiltins.h"
#include "kjson/KjNode.h"
#include "kjson/kjson.h"
#include "kjson/kjBuilder.h"
#include "kjson/kjLookup.h"
#include "kjson/kjRender.h"
#include "kjson/kjRenderSize.h"
#include "kjson/kjClone.h"
#include "kjson/kjFree.h"
#include "kalloc/kalloc.h"
#include "kalloc/kaAlloc.h"
#include "ktrace/kTrace.h"
#include "swRest/swRest.h"



// -----------------------------------------------------------------------------
//
// Globals
//
static KjNode* dumpArray     = NULL;     // accumulates requests (malloc allocator)
static int     dumpCount     = 0;



// -----------------------------------------------------------------------------
//
// Programmable response stubs (the mock-reply API)
//
// A test POSTs to /mock/reply to program how ftClient answers a forwarded
// request — { "verb": "POST", "path": "/attrs/", "status": 207, "body": {...} }.
// On each forwarded request the stub list is consulted (verb match + path
// substring); the first match's status + body is served, else the --status
// fallback applies. Stubs are persistent until DELETE /mock/reply.
//
typedef struct FtStub
{
  char*           verb;     // "POST", "GET", ...
  char*           path;     // substring to find in the request urlPath
  int             status;   // HTTP status to return
  char*           body;     // rendered JSON body to return (malloc), or NULL
  struct FtStub*  next;
} FtStub;

static FtStub* ftStubs = NULL;



// -----------------------------------------------------------------------------
//
// Command line arguments
//
unsigned short ftPort       = 7701;
bool           ftFg         = true;
unsigned short ftPostStatus = 200;     // status returned for accumulate POSTs; override with --status
unsigned int   ftDelayMs    = 0;       // sleep before responding — for timeout tests
unsigned short ftMqttPort   = 0;       // 0 = no MQTT subscription
char*          ftMqttTopic  = (char*) "#"; // default: catch every topic
char*          ftHttpsKey   = NULL;    // path to a PEM private key  (enables TLS when both set)
char*          ftHttpsCert  = NULL;    // path to a PEM certificate  (enables TLS when both set)

static KArg ftArgV[] =
{
  { "--port",            "-p",  KaUShort, _vp &ftPort,       KaOpt, _vp 7701,  _vp 1, _vp 65535, "TCP port to listen on" },
  { "--foreground",      "-fg", KaBool,   _vp &ftFg,         KaOpt, _vp KTRUE, _vp KFALSE, _vp KTRUE, "run in foreground" },
  { "--status",          "-s",  KaUShort, _vp &ftPostStatus, KaOpt, _vp 200,   _vp 100, _vp 599, "HTTP status for accumulate POSTs (misbehave mode)" },
  { "--delay",           NULL,  KaUInt,   _vp &ftDelayMs,    KaOpt, _vp 0,     _vp 0, _vp 600000, "sleep N ms before responding (timeout tests)" },
  { "--mqttPort",        NULL,  KaUShort, _vp &ftMqttPort,   KaOpt, _vp 0,     _vp 0, _vp 65535, "MQTT broker port to subscribe to (0 = disabled)" },
  { "--mqttTopic",       NULL,  KaString, _vp &ftMqttTopic,  KaOpt, _vp "#",   NULL,  NULL,      "MQTT topic to subscribe (default '#')" },
  { "--httpsKey",        "-k",  KaString, _vp &ftHttpsKey,   KaOpt, NULL,      NULL,  NULL,      "PEM private key file (serve HTTPS; needs --httpsCertificate)" },
  { "--httpsCertificate","-c",  KaString, _vp &ftHttpsCert,  KaOpt, NULL,      NULL,  NULL,      "PEM certificate file (serve HTTPS; needs --httpsKey)" },
  KARGS_END
};



// -----------------------------------------------------------------------------
//
// dumpInit - create the accumulator array
//
static void dumpInit(void)
{
  dumpArray = kjArray(NULL, NULL);
  dumpCount = 0;
}



// -----------------------------------------------------------------------------
//
// dumpClear - free all accumulated entries and reset
//
static void dumpClear(void)
{
  if (dumpArray != NULL)
    kjFree(dumpArray);

  dumpInit();
}



// -----------------------------------------------------------------------------
//
// dumpAccumulate - record an incoming request
//
// Stores: { "verb": "POST", "url": "/notify", "payload": "..." }
// Uses malloc allocator (NULL) so entries persist across requests.
//
static void dumpAccumulate(void)
{
  //
  // All strings come from per-request allocators that will be freed.
  // Use kjString with NULL allocator (malloc) — it strdup's the value.
  //
  KjNode* entry = kjObject(NULL, NULL);

  kjChildAdd(entry, kjString(NULL, "verb",    swRest.in.verbString ? swRest.in.verbString : "?"));
  kjChildAdd(entry, kjString(NULL, "url",     swRest.in.urlPath    ? swRest.in.urlPath    : "?"));

  // Headers
  KjNode* hdrs = kjObject(NULL, "headers");
  for (int i = 0; i < swRest.in.httpHeaderCount; i++)
    kjChildAdd(hdrs, kjString(NULL, swRest.in.httpHeaderV[i].key, swRest.in.httpHeaderV[i].value));
  kjChildAdd(entry, hdrs);

  // URI params
  if (swRest.in.uriParamCount > 0)
  {
    KjNode* params = kjObject(NULL, "params");
    for (int i = 0; i < swRest.in.uriParamCount; i++)
      kjChildAdd(params, kjString(NULL, swRest.in.uriParamV[i].key, swRest.in.uriParamV[i].value));
    kjChildAdd(entry, params);
  }

  // Body — deep-clone the request tree since the original is per-request allocated
  if (swRest.in.requestTree != NULL)
  {
    KjNode* bodyClone = kjClone(NULL, swRest.in.requestTree);
    if (bodyClone != NULL)
    {
      bodyClone->name = (char*) "body";
      kjChildAdd(entry, bodyClone);
    }
  }
  else if (swRest.in.payload != NULL && swRest.in.payloadSize > 0)
    kjChildAdd(entry, kjString(NULL, "body", swRest.in.payload));

  kjChildAdd(dumpArray, entry);
  dumpCount++;
}



// -----------------------------------------------------------------------------
//
// getDump - GET /dump — return all accumulated requests as a JSON array
//
static bool getDump(void)
{
  if (dumpArray == NULL || dumpCount == 0)
  {
    // Return empty array
    swRest.out.payload     = (char*) "[]";
    swRest.out.payloadSize = 2;
    return true;
  }

  //
  // Render dumpArray to JSON.
  // Use a scratch buffer allocated via malloc so the output survives
  // beyond the per-request kalloc lifetime — swRest copies the response
  // body (MHD_RESPMEM_MUST_COPY), so we can free this buffer after
  // the call returns by using kaAlloc from the per-request allocator.
  //
  int   bufSize = kjFastRenderSize(dumpArray) + 1;
  char* buf     = (char*) kaAlloc(&swRest.kalloc, bufSize);

  kjFastRender(dumpArray, buf);

  swRest.out.payload     = buf;
  swRest.out.payloadSize = strlen(buf);

  return true;
}



// -----------------------------------------------------------------------------
//
// deleteDump - DELETE /dump — clear the accumulator, return 204
//
static bool deleteDump(void)
{
  dumpClear();
  swRest.out.httpStatusCode = 204;
  return true;
}



// -----------------------------------------------------------------------------
//
// getDie - GET /die — exit the process
//
static bool getDie(void)
{
  swRest.out.httpStatusCode = 200;
  swRest.out.payload        = (char*) "bye";
  swRest.out.payloadSize    = 3;

  // Schedule exit after the response is sent
  // (MHD_RESPMEM_MUST_COPY means swRest copies the buffer before we exit)
  _exit(0);

  return true;  // unreachable
}



// -----------------------------------------------------------------------------
//
// ftStubMatch - first stub matching the verb (exact) and path (substring)
//
static FtStub* ftStubMatch(const char* verb, const char* path)
{
  if (verb == NULL || path == NULL)
    return NULL;

  for (FtStub* s = ftStubs; s != NULL; s = s->next)
    if ((strcmp(s->verb, verb) == 0) && (strstr(path, s->path) != NULL))
      return s;

  return NULL;
}



// -----------------------------------------------------------------------------
//
// stubServe - if a stub matches the current request, serve it (return true)
//
static bool stubServe(void)
{
  FtStub* s = ftStubMatch(swRest.in.verbString, swRest.in.urlPath);
  if (s == NULL)
    return false;

  swRest.out.httpStatusCode = s->status;

  if (s->body != NULL)
  {
    int   n   = strlen(s->body);
    char* buf = (char*) kaAlloc(&swRest.kalloc, n + 1);
    memcpy(buf, s->body, n + 1);
    swRest.out.payload     = buf;
    swRest.out.payloadSize = n;
  }

  KT_T(1, "stub served: %s %s -> %d (bodyLen=%d)", s->verb, swRest.in.urlPath, s->status, s->body ? (int) strlen(s->body) : 0);
  return true;
}



// -----------------------------------------------------------------------------
//
// postMockReply - POST /mock/reply — program a stubbed response
//
// Body: { "verb": "POST", "path": "/attrs/", "status": 207, "body": {...} }
// verb/path/status default to "POST" / "/" / 200; body is optional.
//
static bool postMockReply(void)
{
  KjNode* tree = swRest.in.requestTree;
  if (tree == NULL)
  {
    swRest.out.httpStatusCode = 400;
    return true;
  }

  KjNode* verbP   = kjLookup(tree, "verb");
  KjNode* pathP   = kjLookup(tree, "path");
  KjNode* statusP = kjLookup(tree, "status");
  KjNode* bodyP   = kjLookup(tree, "body");
  KjNode* rawP    = kjLookup(tree, "raw");   // verbatim body (e.g. malformed JSON for a 502 test)

  FtStub* s = (FtStub*) calloc(1, sizeof(FtStub));
  s->verb   = strdup(((verbP != NULL) && (verbP->type == KjString)) ? verbP->value.s : "POST");
  s->path   = strdup(((pathP != NULL) && (pathP->type == KjString)) ? pathP->value.s : "/");
  s->status = ((statusP != NULL) && (statusP->type == KjInt)) ? (int) statusP->value.i : 200;
  s->body   = NULL;

  if ((rawP != NULL) && (rawP->type == KjString))
    s->body = strdup(rawP->value.s);
  else if (bodyP != NULL)
  {
    bodyP->name = NULL;   // render the value alone, not  "body": {...}
    int n = kjFastRenderSize(bodyP) + 1;
    s->body = (char*) malloc(n);
    kjFastRender(bodyP, s->body);
  }

  s->next = ftStubs;
  ftStubs = s;

  KT_T(1, "stub programmed: %s <path-contains %s> -> %d", s->verb, s->path, s->status);
  swRest.out.httpStatusCode = 201;
  return true;
}



// -----------------------------------------------------------------------------
//
// deleteMockReply - DELETE /mock/reply — clear all programmed stubs
//
static bool deleteMockReply(void)
{
  while (ftStubs != NULL)
  {
    FtStub* next = ftStubs->next;
    free(ftStubs->verb);
    free(ftStubs->path);
    free(ftStubs->body);
    free(ftStubs);
    ftStubs = next;
  }

  swRest.out.httpStatusCode = 204;
  return true;
}



// -----------------------------------------------------------------------------
//
// postAccumulate - POST /** — catch-all, accumulate the request, return 200
//
static bool postAccumulate(void)
{
  dumpAccumulate();
  KT_T(1, "POST %s received (total: %d, status=%u, delay=%ums)", swRest.in.urlPath, dumpCount, ftPostStatus, ftDelayMs);

  if (ftDelayMs > 0)
    usleep(ftDelayMs * 1000);

  // A programmed stub (POST /mock/reply) wins over the --status fallback.
  if (stubServe())
    return true;

  swRest.out.httpStatusCode = ftPostStatus;

  //
  // For 4xx/5xx responses, emit an NGSI-LD ProblemDetails body so swBroker's
  // forward-failure surfacing has something spec-shaped to log / include
  // as the "reason" in notCreated entries.
  //
  if (ftPostStatus >= 400)
  {
    static char errBuf[256];
    snprintf(errBuf, sizeof(errBuf),
             "{\"type\":\"https://uri.etsi.org/ngsi-ld/errors/InternalError\","
             "\"title\":\"Mock Error\","
             "\"detail\":\"ftClient configured with --status %u\"}", ftPostStatus);
    swRest.out.payload     = errBuf;
    swRest.out.payloadSize = strlen(errBuf);
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// getAccumulate - GET /** — catch-all for forwarded queries (queryEntities,
//                 retrieveEntity), accumulates the request and returns an
//                 empty entity array. Honors --status like postAccumulate
//                 so timeout / error simulations work for GETs too.
//
// /info/sourceIdentity is a discovery probe (broker learns the CSR's
// alias for loop-detection at registration time) — infrastructure, not
// a forwarded operation. Don't pollute the dump with it.
//
static bool getAccumulate(void)
{
  bool isProbe = (swRest.in.urlPath != NULL &&
                  strcmp(swRest.in.urlPath, "/info/sourceIdentity") == 0);

  if (!isProbe)
    dumpAccumulate();
  KT_T(1, "GET %s received (probe=%d total: %d, status=%u, delay=%ums)",
       swRest.in.urlPath, isProbe, dumpCount, ftPostStatus, ftDelayMs);

  if (ftDelayMs > 0)
    usleep(ftDelayMs * 1000);

  // A programmed stub (POST /mock/reply) wins over the --status fallback.
  if (stubServe())
    return true;

  swRest.out.httpStatusCode = (ftPostStatus == 201) ? 200 : ftPostStatus;

  if (ftPostStatus >= 400)
  {
    static char errBuf[256];
    snprintf(errBuf, sizeof(errBuf),
             "{\"type\":\"https://uri.etsi.org/ngsi-ld/errors/InternalError\","
             "\"title\":\"Mock Error\","
             "\"detail\":\"ftClient configured with --status %u\"}", ftPostStatus);
    swRest.out.payload     = errBuf;
    swRest.out.payloadSize = strlen(errBuf);
  }
  else
  {
    swRest.out.payload     = (char*) "[]";
    swRest.out.payloadSize = 2;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// MQTT subscriber — runs in its own thread, accumulates received messages
// into the same dumpArray under verb="MQTT", url=<topic>, body=<payload>.
//
static pthread_mutex_t mqttDumpMutex = PTHREAD_MUTEX_INITIALIZER;

static void mqttOnConnect(struct mosquitto* m, void* ud, int rc)
{
  (void) ud; (void) rc;
  mosquitto_subscribe(m, NULL, ftMqttTopic, 0);
}

static void mqttOnMessage(struct mosquitto* m, void* ud, const struct mosquitto_message* msg)
{
  (void) m; (void) ud;
  if (msg == NULL || msg->payload == NULL) return;

  KjNode* entry = kjObject(NULL, NULL);
  kjChildAdd(entry, kjString(NULL, "verb",  "MQTT"));
  kjChildAdd(entry, kjString(NULL, "url",   msg->topic ? msg->topic : ""));

  // Try to parse payload as JSON; fall back to a string body.
  char* payloadStr = (char*) malloc(msg->payloadlen + 1);
  if (payloadStr == NULL) return;
  memcpy(payloadStr, msg->payload, msg->payloadlen);
  payloadStr[msg->payloadlen] = '\0';

  // Store the payload as a string body — same shape as HTTP non-JSON
  // bodies. Tests typically grep on the payload text directly.
  kjChildAdd(entry, kjString(NULL, "body", payloadStr));
  free(payloadStr);

  pthread_mutex_lock(&mqttDumpMutex);
  kjChildAdd(dumpArray, entry);
  dumpCount++;
  pthread_mutex_unlock(&mqttDumpMutex);
}

static void* mqttListenerThread(void* arg)
{
  (void) arg;

  if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS)
    return NULL;

  struct mosquitto* mosq = mosquitto_new(NULL, true, NULL);
  if (mosq == NULL) return NULL;

  mosquitto_connect_callback_set(mosq, mqttOnConnect);
  mosquitto_message_callback_set(mosq, mqttOnMessage);

  // Reconnect-loop friendly: try forever (the broker may not be up yet).
  while (mosquitto_connect(mosq, "localhost", ftMqttPort, 60) != MOSQ_ERR_SUCCESS)
    usleep(200000);

  mosquitto_loop_forever(mosq, -1, 1);

  mosquitto_destroy(mosq);
  mosquitto_lib_cleanup();
  return NULL;
}



// -----------------------------------------------------------------------------
//
// Service table
//
static SwRestServiceSimplified ftServices[] =
{
  { SwVerbGet,    "/dump",  getDump,         0,                       0 },
  { SwVerbDelete, "/dump",  deleteDump,      0,                       0 },
  { SwVerbGet,    "/die",   getDie,          0,                       0 },
  // Programmable response stubs (mock-reply API).
  { SwVerbPost,   "/mock/reply", postMockReply,   ~(uint64_t)0,       0 },
  { SwVerbDelete, "/mock/reply", deleteMockReply, 0,                  0 },
  // Catch-all accumulators — every verb lands here and honors --status.
  // supportedParams = ~0ULL: ftClient mocks any NGSI-LD endpoint and
  // must accept whatever URL params the broker forwards, without 400ing.
  { SwVerbGet,    "/**",    getAccumulate,   ~(uint64_t)0,            0 },
  { SwVerbPost,   "/**",    postAccumulate,  ~(uint64_t)0,            0 },
  { SwVerbDelete, "/**",    postAccumulate,  ~(uint64_t)0,            0 },
  { SwVerbPatch,  "/**",    postAccumulate,  ~(uint64_t)0,            0 },
  { SwVerbPut,    "/**",    postAccumulate,  ~(uint64_t)0,            0 },
};

static int ftServiceCount = sizeof(ftServices) / sizeof(ftServices[0]);



// -----------------------------------------------------------------------------
//
// pemSlurp - read a PEM file into a malloc'd, NUL-terminated string
//
// swRest's HTTPS server options want the key/certificate as in-memory PEM, not
// file paths. Returns NULL (and prints to stderr) on any error.
//
static char* pemSlurp(const char* path)
{
  FILE* f = fopen(path, "rb");
  if (f == NULL)
  {
    fprintf(stderr, "ftClient: cannot open '%s'\n", path);
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if ((size <= 0) || (size > 64 * 1024))
  {
    fprintf(stderr, "ftClient: '%s' has unreasonable size %ld\n", path, size);
    fclose(f);
    return NULL;
  }

  char* buf = (char*) malloc(size + 1);
  size_t n = fread(buf, 1, size, f);
  fclose(f);

  if (n != (size_t) size)
  {
    fprintf(stderr, "ftClient: short read on '%s'\n", path);
    free(buf);
    return NULL;
  }

  buf[size] = 0;
  return buf;
}



// -----------------------------------------------------------------------------
//
// main
//
int main(int argC, char* argV[])
{
  char* progName = strrchr(argV[0], '/');
  progName = (progName != NULL) ? progName + 1 : argV[0];

  KArgsStatus ks = kargsInit(progName, ftArgV, "FTCLIENT");
  if (ks != KargsOk)
  {
    fprintf(stderr, "kargsInit failed\n");
    return 1;
  }

  ks = kargsParse(argC, argV);
  if (ks != KargsOk)
  {
    fprintf(stderr, "kargsParse failed\n");
    return 1;
  }

  if (ktInit("ftClient", "/tmp", false, NULL, "0-255", kaBuiltinVerbose, kaBuiltinDebug, false) != 0)
  {
    fprintf(stderr, "ktInit failed\n");
    return 1;
  }

  signal(SIGINT,  SIG_DFL);
  signal(SIGTERM, SIG_DFL);

  dumpInit();

  swRestSetPrettySpaces(2);

  // --httpsKey + --httpsCertificate → serve notifications over TLS
  if ((ftHttpsKey != NULL) && (ftHttpsCert != NULL))
  {
    char* keyPem  = pemSlurp(ftHttpsKey);
    char* certPem = pemSlurp(ftHttpsCert);

    if ((keyPem == NULL) || (certPem == NULL))
      return 1;

    swRestHttpsServerCredentialsSet(keyPem, certPem);
    KT_I("ftClient serving HTTPS on port %u", ftPort);
  }

  if (swRestInit(ftServices, ftServiceCount, ftPort, 2) != 0)
  {
    fprintf(stderr, "ftClient: swRestInit failed on port %u\n", ftPort);
    return 1;
  }

  if (ftMqttPort != 0)
  {
    pthread_t tid;
    if (pthread_create(&tid, NULL, mqttListenerThread, NULL) != 0)
    {
      fprintf(stderr, "ftClient: failed to start MQTT listener thread\n");
      return 1;
    }
    pthread_detach(tid);
    KT_I("ftClient MQTT listener: localhost:%u topic='%s'", ftMqttPort, ftMqttTopic);
  }

  KT_I("ftClient running on port %u", ftPort);

  while (1)
    pause();

  return 0;
}

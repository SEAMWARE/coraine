//
// FILE            ftClient.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
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
#include "corRest/corRest.h"



// -----------------------------------------------------------------------------
//
// Globals
//
static KjNode* dumpArray     = NULL;     // accumulates requests (malloc allocator)
static int     dumpCount     = 0;
static int     probeCount    = 0;        // sourceIdentity discovery probes seen (kept OUT of dumpArray)

//
// dumpMutex - guards dumpArray + dumpCount, on every path that touches them.
//
// ftClient is thread-per-connection, so two notifications in flight at once run
// dumpAccumulate on two threads and spliced into the same tail unguarded: the
// order of the two entries flipped, and one of them could be lost outright.
// GET /dump rendered the list while it was being appended to, and DELETE /dump
// kjFree'd it out from under a writer.
//
// The mutex existed already - as mqttDumpMutex, taken only by the MQTT
// listener, which shares this very array with the HTTP path. It was never
// MQTT's mutex; it guards the accumulator, and it is named for that now.
//
static pthread_mutex_t dumpMutex = PTHREAD_MUTEX_INITIALIZER;



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
  int             delayMs;  // sleep this many ms before responding (per-path timeout injection), 0 = none
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
//
// ftMqttSubscribed - set when the MQTT SUBACK arrives, read by GET /mqttReady.
//
// Not "the thread started" and not "subscribe was called": a publish that
// reaches the broker before the SUBACK is delivered to nobody and is DISCARDED,
// not queued. So the only readiness that means anything to a test is the
// acknowledgement, which is why this is set in the on-subscribe callback.
//
static volatile bool ftMqttSubscribed = false;

//
// ftMqttFailed - set when the MQTT connection is given up on for good.
//
// The connect loop used to be unbounded ("try forever, the broker may not be up
// yet"), which is right for a broker that is merely slow and wrong for one that
// is never coming: ftClient span silently, printed nothing, and the only symptom
// was a readiness barrier timing out with an EMPTY log to show for it. A bounded
// retry that says why is strictly better - the failure is reported by the process
// that actually observed it.
//
static volatile bool ftMqttFailed = false;

unsigned short ftMqttPort   = 0;       // 0 = no MQTT subscription
char*          ftMqttTopic  = (char*) "#"; // default: catch every topic
char*          ftMqttUser   = NULL;    // MQTT username (for an auth-required broker)
char*          ftMqttPass   = NULL;    // MQTT password
bool           ftMqttTls    = false;   // subscribe over TLS (mqtts), insecure
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
  { "--mqttUser",        NULL,  KaString, _vp &ftMqttUser,   KaOpt, NULL,      NULL,  NULL,      "MQTT username (auth-required broker)" },
  { "--mqttPassword",    NULL,  KaString, _vp &ftMqttPass,   KaOpt, NULL,      NULL,  NULL,      "MQTT password" },
  { "--mqttTls",         NULL,  KaBool,   _vp &ftMqttTls,    KaOpt, _vp KFALSE, _vp KFALSE, _vp KTRUE, "subscribe over TLS (mqtts), accept self-signed" },
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
  dumpArray  = kjArray(NULL, NULL);
  dumpCount  = 0;
  probeCount = 0;
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

  kjChildAdd(entry, kjString(NULL, "verb",    corRest.in.verbString ? corRest.in.verbString : "?"));
  kjChildAdd(entry, kjString(NULL, "url",     corRest.in.urlPath    ? corRest.in.urlPath    : "?"));

  // Headers
  KjNode* hdrs = kjObject(NULL, "headers");
  for (int i = 0; i < corRest.in.httpHeaderCount; i++)
    kjChildAdd(hdrs, kjString(NULL, corRest.in.httpHeaderV[i].key, corRest.in.httpHeaderV[i].value));
  kjChildAdd(entry, hdrs);

  // URI params
  if (corRest.in.uriParamCount > 0)
  {
    KjNode* params = kjObject(NULL, "params");
    for (int i = 0; i < corRest.in.uriParamCount; i++)
      kjChildAdd(params, kjString(NULL, corRest.in.uriParamV[i].key, corRest.in.uriParamV[i].value));
    kjChildAdd(entry, params);
  }

  // Body — deep-clone the request tree since the original is per-request allocated
  if (corRest.in.requestTree != NULL)
  {
    KjNode* bodyClone = kjClone(NULL, corRest.in.requestTree);
    if (bodyClone != NULL)
    {
      bodyClone->name = (char*) "body";
      kjChildAdd(entry, bodyClone);
    }
  }
  else if (corRest.in.payload != NULL && corRest.in.payloadSize > 0)
    kjChildAdd(entry, kjString(NULL, "body", corRest.in.payload));

  pthread_mutex_lock(&dumpMutex);
  kjChildAdd(dumpArray, entry);
  dumpCount++;
  pthread_mutex_unlock(&dumpMutex);
}



// -----------------------------------------------------------------------------
//
// getDump - GET /dump — return all accumulated requests as a JSON array
//
static bool getDump(void)
{
  pthread_mutex_lock(&dumpMutex);

  if (dumpArray == NULL || dumpCount == 0)
  {
    // Return empty array
    corRest.out.payload     = (char*) "[]";
    corRest.out.payloadSize = 2;
    pthread_mutex_unlock(&dumpMutex);
    return true;
  }

  //
  // Render dumpArray to JSON.
  // Use a scratch buffer allocated via malloc so the output survives
  // beyond the per-request kalloc lifetime — corRest copies the response
  // body (MHD_RESPMEM_MUST_COPY), so we can free this buffer after
  // the call returns by using kaAlloc from the per-request allocator.
  //
  int   bufSize = kjFastRenderSize(dumpArray) + 1;
  char* buf     = (char*) kaAlloc(&corRest.kalloc, bufSize);

  kjFastRender(dumpArray, buf);

  pthread_mutex_unlock(&dumpMutex);

  corRest.out.payload     = buf;
  corRest.out.payloadSize = strlen(buf);

  return true;
}



// -----------------------------------------------------------------------------
//
// deleteDump - DELETE /dump — clear the accumulator, return 204
//
static bool deleteDump(void)
{
  pthread_mutex_lock(&dumpMutex);
  dumpClear();
  pthread_mutex_unlock(&dumpMutex);

  corRest.out.httpStatusCode = 204;
  return true;
}



// -----------------------------------------------------------------------------
//
// getCount - GET /count — number of accumulated requests as a bare integer
//
// A parser-free alternative to GET /dump for the (many) tests that only need
// the count of received requests: returns e.g. "3" (or "0" when empty) as plain
// text, never JSON. A caller reads it with plain curl and never has to pipe a
// possibly-empty/invalid body through a JSON parser — which, on an empty dump,
// would throw to stderr and flake the test under parallel load.
//
static bool getCount(void)
{
  char* buf = (char*) kaAlloc(&corRest.kalloc, 16);

  pthread_mutex_lock(&dumpMutex);
  snprintf(buf, 16, "%d", dumpCount);
  pthread_mutex_unlock(&dumpMutex);

  corRest.out.payload     = buf;
  corRest.out.payloadSize = strlen(buf);

  return true;
}



// -----------------------------------------------------------------------------
//
// getMqttReady - GET /mqttReady — 1 once the MQTT SUBACK has arrived.
//
// A bare integer, like /count: the harness polls this from bash and a JSON
// parser has no business in a readiness barrier. Answers 1 when no MQTT was
// asked for, so a caller need not special-case that.
//
// Three answers, not two:
//   1  subscribed, or none was asked for
//   0  not yet - keep polling
//  -1  given up on; polling longer cannot help, and the log says why
//
// -1 is what turns a barrier timeout into an immediate, explained failure.
//
static bool getMqttReady(void)
{
  char* buf = (char*) kaAlloc(&corRest.kalloc, 4);

  int state = ((ftMqttPort == 0) || (ftMqttSubscribed == true))?  1 :
              (ftMqttFailed == true)?                            -1 : 0;

  snprintf(buf, 4, "%d", state);
  corRest.out.payload     = buf;
  corRest.out.payloadSize = strlen(buf);

  return true;
}



// -----------------------------------------------------------------------------
//
// getProbeCount - GET /probeCount — number of sourceIdentity probes seen.
//
// Probes are infrastructure (§ 5.15 alias discovery) and are deliberately kept
// out of the request dump, so a test can't count them there. This bare-integer
// endpoint lets a test assert the probe fired (no contextSourceAlias supplied)
// or did NOT fire (alias supplied in the registration → probe skipped).
//
static bool getProbeCount(void)
{
  char* buf = (char*) kaAlloc(&corRest.kalloc, 16);

  snprintf(buf, 16, "%d", probeCount);
  corRest.out.payload     = buf;
  corRest.out.payloadSize = strlen(buf);

  return true;
}



// -----------------------------------------------------------------------------
//
// getDie - GET /die — exit the process
//
static bool getDie(void)
{
  corRest.out.httpStatusCode = 200;
  corRest.out.payload        = (char*) "bye";
  corRest.out.payloadSize    = 3;

  // Schedule exit after the response is sent
  // (MHD_RESPMEM_MUST_COPY means corRest copies the buffer before we exit)
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
  FtStub* s = ftStubMatch(corRest.in.verbString, corRest.in.urlPath);
  if (s == NULL)
    return false;

  // Per-path delay — lets a test time out ONLY a specific endpoint (e.g. the
  // sourceIdentity probe) without the global --delay slowing the forwards too.
  if (s->delayMs > 0)
    usleep(s->delayMs * 1000);

  corRest.out.httpStatusCode = s->status;

  if (s->body != NULL)
  {
    int   n   = strlen(s->body);
    char* buf = (char*) kaAlloc(&corRest.kalloc, n + 1);
    memcpy(buf, s->body, n + 1);
    corRest.out.payload     = buf;
    corRest.out.payloadSize = n;
  }

  KT_T(1, "stub served: %s %s -> %d (bodyLen=%d)", s->verb, corRest.in.urlPath, s->status, s->body ? (int) strlen(s->body) : 0);
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
  KjNode* tree = corRest.in.requestTree;
  if (tree == NULL)
  {
    corRest.out.httpStatusCode = 400;
    return true;
  }

  KjNode* verbP   = kjLookup(tree, "verb");
  KjNode* pathP   = kjLookup(tree, "path");
  KjNode* statusP = kjLookup(tree, "status");
  KjNode* delayP  = kjLookup(tree, "delayMs"); // sleep before responding (per-path timeout injection)
  KjNode* bodyP   = kjLookup(tree, "body");
  KjNode* rawP    = kjLookup(tree, "raw");   // verbatim body (e.g. malformed JSON for a 502 test)

  FtStub* s = (FtStub*) calloc(1, sizeof(FtStub));
  s->verb    = strdup(((verbP != NULL) && (verbP->type == KjString)) ? verbP->value.s : "POST");
  s->path    = strdup(((pathP != NULL) && (pathP->type == KjString)) ? pathP->value.s : "/");
  s->status  = ((statusP != NULL) && (statusP->type == KjInt)) ? (int) statusP->value.i : 200;
  s->delayMs = ((delayP  != NULL) && (delayP->type  == KjInt)) ? (int) delayP->value.i  : 0;
  s->body    = NULL;

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
  corRest.out.httpStatusCode = 201;
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

  corRest.out.httpStatusCode = 204;
  return true;
}



// -----------------------------------------------------------------------------
//
// postAccumulate - POST /** — catch-all, accumulate the request, return 200
//
static bool postAccumulate(void)
{
  dumpAccumulate();
  KT_T(1, "POST %s received (total: %d, status=%u, delay=%ums)", corRest.in.urlPath, dumpCount, ftPostStatus, ftDelayMs);

  if (ftDelayMs > 0)
    usleep(ftDelayMs * 1000);

  // A programmed stub (POST /mock/reply) wins over the --status fallback.
  if (stubServe())
    return true;

  corRest.out.httpStatusCode = ftPostStatus;

  //
  // For 4xx/5xx responses, emit an NGSI-LD ProblemDetails body so coraine's
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
    corRest.out.payload     = errBuf;
    corRest.out.payloadSize = strlen(errBuf);
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
  // Match the sourceIdentity probe by SUFFIX: the resource lives under the API
  // root, so the broker sends "<endpoint>/ngsi-ld/v1/info/sourceIdentity". A
  // bare-path match (old "/info/sourceIdentity") silently stops recognizing the
  // probe once the caller carries the /ngsi-ld/v1 prefix, letting it pollute the
  // dump. Suffix-match covers both spellings.
  static const char probeSuffix[] = "/info/sourceIdentity";
  bool isProbe = false;
  if (corRest.in.urlPath != NULL)
  {
    int urlLen    = (int) strlen(corRest.in.urlPath);
    int suffixLen = (int) (sizeof(probeSuffix) - 1);
    isProbe = (urlLen >= suffixLen) &&
              (strcmp(corRest.in.urlPath + urlLen - suffixLen, probeSuffix) == 0);
  }

  if (isProbe)
    probeCount++;
  else
    dumpAccumulate();
  KT_T(1, "GET %s received (probe=%d total: %d, status=%u, delay=%ums)",
       corRest.in.urlPath, isProbe, dumpCount, ftPostStatus, ftDelayMs);

  if (ftDelayMs > 0)
    usleep(ftDelayMs * 1000);

  // A programmed stub (POST /mock/reply) wins over the --status fallback.
  if (stubServe())
    return true;

  corRest.out.httpStatusCode = (ftPostStatus == 201) ? 200 : ftPostStatus;

  if (ftPostStatus >= 400)
  {
    static char errBuf[256];
    snprintf(errBuf, sizeof(errBuf),
             "{\"type\":\"https://uri.etsi.org/ngsi-ld/errors/InternalError\","
             "\"title\":\"Mock Error\","
             "\"detail\":\"ftClient configured with --status %u\"}", ftPostStatus);
    corRest.out.payload     = errBuf;
    corRest.out.payloadSize = strlen(errBuf);
  }
  else
  {
    corRest.out.payload     = (char*) "[]";
    corRest.out.payloadSize = 2;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// MQTT subscriber — runs in its own thread, accumulates received messages
// into the same dumpArray under verb="MQTT", url=<topic>, body=<payload>.
//
static void mqttOnConnect(struct mosquitto* m, void* ud, int rc)
{
  (void) ud;

  //
  // rc is the CONNACK return code, and it was being ignored. A refused CONNECT -
  // bad credentials, most usefully - completes the TCP connection and then fails
  // at the MQTT level, so subscribing anyway produced no SUBACK and no reason.
  //
  if (rc != 0)
  {
    fprintf(stderr, "ftClient: MQTT broker on port %d refused the connection: %s\n",
            ftMqttPort, mosquitto_connack_string(rc));
    fflush(stderr);
    ftMqttFailed = true;
    return;
  }

  mosquitto_subscribe(m, NULL, ftMqttTopic, 0);
}



// -----------------------------------------------------------------------------
//
// mqttOnSubscribe - the SUBACK. From here on, a publish on the topic arrives.
//
static void mqttOnSubscribe(struct mosquitto* m, void* ud, int mid, int qosCount, const int* grantedQos)
{
  (void) m; (void) ud; (void) mid; (void) qosCount; (void) grantedQos;
  ftMqttSubscribed = true;
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

  pthread_mutex_lock(&dumpMutex);
  kjChildAdd(dumpArray, entry);
  dumpCount++;
  pthread_mutex_unlock(&dumpMutex);
}

static void* mqttListenerThread(void* arg)
{
  (void) arg;

  if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS)
    return NULL;

  struct mosquitto* mosq = mosquitto_new(NULL, true, NULL);
  if (mosq == NULL) return NULL;

  if (ftMqttUser != NULL)
    mosquitto_username_pw_set(mosq, ftMqttUser, ftMqttPass);

  // --mqttTls: subscribe over TLS, accepting a self-signed broker cert
  // (test rig only — same insecure posture as the broker's --insecureNotif).
  if (ftMqttTls)
  {
    mosquitto_tls_set(mosq, NULL, "/etc/ssl/certs", NULL, NULL, NULL);
    mosquitto_tls_opts_set(mosq, 0 /* SSL_VERIFY_NONE */, NULL, NULL);
    mosquitto_tls_insecure_set(mosq, true);
  }

  mosquitto_connect_callback_set(mosq, mqttOnConnect);
  mosquitto_subscribe_callback_set(mosq, mqttOnSubscribe);
  mosquitto_message_callback_set(mosq, mqttOnMessage);

  //
  // Retry, but not forever. The broker may legitimately not be up yet, so this
  // keeps trying - and if it never appears, it says so and stops, rather than
  // spinning in silence and leaving the reader of the log nothing to go on.
  //
  // 5 seconds: far longer than a local mosquitto needs even on a loaded runner,
  // and comfortably inside the readiness barrier that is waiting on this, so the
  // barrier hears the verdict instead of timing out over the top of it.
  //
  int rc = MOSQ_ERR_SUCCESS;

  for (int attempt = 0; attempt < 25; attempt++)          // 25 x 200ms = 5s
  {
    rc = mosquitto_connect(mosq, "localhost", ftMqttPort, 60);
    if (rc == MOSQ_ERR_SUCCESS)
      break;
    usleep(200000);
  }

  if (rc != MOSQ_ERR_SUCCESS)
  {
    fprintf(stderr, "ftClient: no MQTT broker on port %d after 5s: %s\n",
            ftMqttPort, mosquitto_strerror(rc));
    fflush(stderr);
    ftMqttFailed = true;
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return NULL;
  }

  mosquitto_loop_forever(mosq, -1, 1);

  mosquitto_destroy(mosq);
  mosquitto_lib_cleanup();
  return NULL;
}



// -----------------------------------------------------------------------------
//
// Service table
//
static CorRestServiceSimplified ftServices[] =
{
  { CorVerbGet,    "/dump",  getDump,         0,                       0 },
  { CorVerbDelete, "/dump",  deleteDump,      0,                       0 },
  { CorVerbGet,    "/count", getCount,        0,                       0 },
  { CorVerbGet,    "/probeCount", getProbeCount, 0,                    0 },
  { CorVerbGet,    "/mqttReady",  getMqttReady,  0,                    0 },
  { CorVerbGet,    "/die",   getDie,          0,                       0 },
  // Programmable response stubs (mock-reply API).
  { CorVerbPost,   "/mock/reply", postMockReply,   ~(uint64_t)0,       0 },
  { CorVerbDelete, "/mock/reply", deleteMockReply, 0,                  0 },
  // Catch-all accumulators — every verb lands here and honors --status.
  // supportedParams = ~0ULL: ftClient mocks any NGSI-LD endpoint and
  // must accept whatever URL params the broker forwards, without 400ing.
  { CorVerbGet,    "/**",    getAccumulate,   ~(uint64_t)0,            0 },
  { CorVerbPost,   "/**",    postAccumulate,  ~(uint64_t)0,            0 },
  { CorVerbDelete, "/**",    postAccumulate,  ~(uint64_t)0,            0 },
  { CorVerbPatch,  "/**",    postAccumulate,  ~(uint64_t)0,            0 },
  { CorVerbPut,    "/**",    postAccumulate,  ~(uint64_t)0,            0 },
};

static int ftServiceCount = sizeof(ftServices) / sizeof(ftServices[0]);



// -----------------------------------------------------------------------------
//
// pemSlurp - read a PEM file into a malloc'd, NUL-terminated string
//
// corRest's HTTPS server options want the key/certificate as in-memory PEM, not
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

  corRestSetPrettySpaces(2);

  // ftClient is a notification RECEIVER — accept geo+json notifications (the
  // broker's § 6.3.4 415 gate otherwise rejects a geo+json POST body).
  corRestAcceptGeoJsonInputSet(true);

  // --httpsKey + --httpsCertificate → serve notifications over TLS
  if ((ftHttpsKey != NULL) && (ftHttpsCert != NULL))
  {
    char* keyPem  = pemSlurp(ftHttpsKey);
    char* certPem = pemSlurp(ftHttpsCert);

    if ((keyPem == NULL) || (certPem == NULL))
      return 1;

    corRestHttpsServerCredentialsSet(keyPem, certPem);
    KT_I("ftClient serving HTTPS on port %u", ftPort);
  }

  if (corRestInit(ftServices, ftServiceCount, ftPort, 2) != 0)
  {
    fprintf(stderr, "ftClient: corRestInit failed on port %u\n", ftPort);
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

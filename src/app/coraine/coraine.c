//
// FILE            coraine.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                              // bool, true, false
#include <stdio.h>                                // snprintf, fprintf
#include <stdlib.h>                               // _exit
#include <unistd.h>                               // pause
#include <signal.h>                               // signal, SIGINT, SIGTERM
#include <string.h>                               // strcmp, memcpy
#include <time.h>                                 // time
#include <stdint.h>                               // uint32_t
#include <execinfo.h>                             // backtrace, backtrace_symbols
#include <ifaddrs.h>                              // getifaddrs, freeifaddrs, struct ifaddrs
#include <net/if.h>                               // IFF_LOOPBACK, IFF_UP, IFF_RUNNING, IFF_POINTOPOINT
#include <sys/socket.h>                           // AF_INET
#include <netinet/in.h>                           // struct sockaddr_in
#include <arpa/inet.h>                            // inet_ntop, ntohl, INET_ADDRSTRLEN

#include "kalloc/kalloc.h"                        // KAlloc, kaBufferInit
#include "ktrace/kTrace.h"                        // KT_I, KT_V, KT_X
#include "kargs/kargs.h"                          // kargsInit, kargsParse, kargsPeek, KArg, KArgsStatus, kargsStatus, KARGS_END, kargsUsage
#include "corPlugin/corPlugin.h"                    // corPluginSetBaseDir, corPluginBaseDir, corPluginArgUpdate
#include "corRest/corRest.h"                        // corRestInit, corRestSetPrettySpaces, corRestSetPreServiceHook, corRestParamAdd
#include "corRest/corRestClient.h"                  // corRestClientInit, CorRestClientRequest/Response
#include "corJsonld/corJsonld.h"                    // corLdInit, CORJSONLD_VERSION
#include "corJsonld/CorLdContext.h"                 // CorLdContext, CorLdContextKind
#include "corJsonld/CorLdContextCache.h"            // CorLdContextCache
#include "corJsonld/corLdCache.h"                   // corLdCacheInsert
#include "corJsonld/corLdContextParse.h"            // corLdContextFromObject

#include "kjson/kjson.h"                          // Kjson
#include "kjson/kjBufferCreate.h"                 // kjBufferCreate
#include "kjson/kjParse.h"                        // kjParse
#include "kjson/kjLookup.h"                       // kjLookup
#include "kjson/kjClone.h"                        // kjClone
#include "kalloc/kaAlloc.h"                       // kaAlloc
#include "kalloc/kaStrdup.h"                      // kaStrdup
#include "corNgsild/corNgsild.h"                    // ldInit, CORNGSILD_VERSION, ldParamsInit
#include "corNgsild/ldUrlWildcardCheck.h"          // ldUrlWildcardCheck
#include "corNgsild/ldHooks.h"                      // ldAcceptPrecondition
#include "corNgsild/ldNotifyDefer.h"               // ldNotifyDispatchPending
#include "corNgsild/ldRegCache.h"                  // ldRegCacheProbePending
#include "corNgsild/ldNotifyStatsHook.h"           // ldNotifyStatsHookSet
#include "corNgsild/ldLinkedEntitiesHook.h"        // ldLinkedEntitiesHookSet
#include "linkedEntities/ldLinkedEntities.h"      // ldLinkedEntitiesNotifApiArray
#include "corNgsild/LdPernotCache.h"               // LdPernotCache, LdPernotItem
#include "corNgsild/ldPernotLoop.h"                // ldPernotLoopStart
#include "corNgsild/ldPeriodicLoop.h"              // ldPeriodicLoopStart, ldPeriodicLoopStop
#include "corNgsild/ldContextHost.h"               // ldContextHostReaperStart
#include "corNgsild/ldCsrSubNotify.h"              // ldCsrSubPeriodicLoopRegister, ldCsrSubDispatchPending
#include "corNgsild/ldCheckSubscription.h"         // ldSubEntityTypeExprsRelease
#include "corNgsild/ldStatsFlushLoop.h"            // ldStatsFlushLoopStart
#include "corNgsild/ldMqttNotify.h"                // ldMqttTlsInsecureSet
#include "metrics/subStatsFlushAll.h"             // subStatsFlushAll
#include "corNgsild/CorNgsild.h"                    // corNgsild, ldCsourceAliasBase
#include "corNgsild/ldError.h"                     // ldError
#include "corNgsild/LdProblem.h"                    // LD_ERROR_BAD_REQUEST_DATA, LD_ERROR_LD_CONTEXT_NOT_AVAILABLE

#include "db/DbDriver.h"                          // db, DB_OK
#include "db/DbQueryFilter.h"                     // DbQueryFilter
#include "db/dbInit.h"                            // dbStart
#include "db/dbClose.h"                           // dbClose
#include "db/Tenant.h"                            // tenantPreServiceHook
#include "db/contextCache.h"                      // contextCacheReload
#include "ha/haInit.h"                            // haInit, haChannel
#include "serviceRoutines/ldSnapshotRead.h"       // ldSnapshotWriteGuard

#include "troe/TroeDriver.h"                      // troe
#include "troe/troeInit.h"                        // troeStart, troeStop
#include "troe/troeDispatch.h"                    // troeDispatchPending
#include "db/dbExpiredEntities.h"                 // dbExpiredEntityDispatchPending

#include "plugin/ApiPlugin.h"                     // ApiPlugin, apiPlugins, apiPluginCount
#include "plugin/pluginLoader.h"                  // pluginLoadDb, pluginLoadApi

#include "forwarding/forwardingHttp.h"            // forwardingHttpRegister

#include "metrics/metrics.h"                      // metricsInit, metricsPreService, metricsPostResponse, metricsNotificationSent, metricsCsrNotificationSent

#include "ngsildServices.h"                       // ngsildCoreServices, serviceBuild



// -----------------------------------------------------------------------------
//
// contextDownload - CorLdDownloadFunction callback for fetching remote @contexts
//
static char* contextDownload(const char* url, int* statusCodeP)
{
  CorRestClientRequest  req;
  CorRestClientResponse resp;

  corRestClientRequestInit(&req, CorVerbGet, url, NULL);
  corRestClientRequestHeader(&req, "Accept", "application/ld+json, application/json");
  corRestClientRequestTimeout(&req, 5000, 10000);

  int r = corRestClientSend(&req, &resp);

  if (r != CORR_OK || resp.statusCode != 200)
  {
    *statusCodeP = (resp.statusCode > 0) ? resp.statusCode : 500;
    corRestClientResponseCleanup(&resp);
    return NULL;
  }

  *statusCodeP = 200;

  // Return a malloc'd copy of the body (corJsonld will free it)
  char* copy = NULL;
  if (resp.body != NULL && resp.bodyLen > 0)
  {
    copy = (char*) malloc(resp.bodyLen + 1);
    memcpy(copy, resp.body, resp.bodyLen);
    copy[resp.bodyLen] = 0;
  }

  corRestClientResponseCleanup(&resp);
  return copy;
}



// -----------------------------------------------------------------------------
//
// contextError - CorLdErrorFunction callback: an @context the library can NAME
//
// corLdContextFromUrl answers NULL for everything, and its callers turn that into
// "unable to retrieve @context" - true for a download that failed, wrong for one
// that downloaded perfectly and is unusable. The library reports the ones it can
// name here, and this turns them into the ProblemDetails the client sees.
//
// corNgsild.contextError is what stops the caller from then overwriting it with
// its own generic answer.
//
static void contextError(int status, const char* title, const char* detail)
{
  const char* type = (status == 400)? LD_ERROR_BAD_REQUEST_DATA : LD_ERROR_LD_CONTEXT_NOT_AVAILABLE;

  ldError(status, type, title, "%s", detail);
  corNgsild.contextError = true;
}



#include "coraineVersion.h"                      // CORAINE_VERSION



// -----------------------------------------------------------------------------
//
// Command line arguments
//
unsigned short port         = 1026;
char*          dbName       = "mongoc";
char*          troeName     = "none";
char*          apiNames     = NULL;
unsigned int   prettySpaces = 0;
bool           notifyValueChangeOnly = false;
bool           fg           = false;
bool           versionOnly  = false;   // --version: handled before kargsInit; in the table so --usage lists it
int            poolSize     = 32;
char*          corsOrigin   = NULL;
int            corsMaxAge   = 86400;
char*          defaultUserContext  = NULL;
char*          csourceAlias = NULL;
char*          httpEndpoint = NULL;
char*          contextSourceExtras = NULL;  // path to a JSON file (§ 5.2.40)
char*          traceLevels  = NULL;
bool           noSplitEntities = false;
bool           distributed     = false;     // --distributed: opt-in distributed operations (like TRoE)
bool           highPrecision   = false;     // --high-precision/-hp: 9-digit (ns) timestamps vs default 6 (µs, §5.2.2.4)
bool           asyncSnapshot   = false;
bool           insecureNotif   = false;     // accept self-signed certs on TLS notifications/forwards
int            maxRequestSize  = 2;          // MiB; § 6.3.2 413 threshold (0 = no cap)
int            subStatsFlushInterval = 60;   // seconds; 0 disables the timer
int            cooldownMillis        = 30000; // --cooldownMillis; default endpoint cooldown after failure (0 = off)

static KArg kargV[] =
{
  { "--traceLevels",        "-t",           KaString, _vp &traceLevels,  KaOpt, _vp NULL,      NULL,  NULL,      "trace levels" },
  { "--port",               "-p",           KaUShort, _vp &port,         KaOpt, _vp 1026,     _vp 1, _vp 65535, "TCP port to listen on" },
  { "--database",           "-db",          KaString, _vp &dbName,       KaOpt, _vp "mongoc", NULL,  NULL,      "database plugin (short name or full path)" },
  { "--troe",               "-troe",        KaString, _vp &troeName,     KaOpt, _vp "none",   NULL,  NULL,      "TRoE temporal-storage plugin (short name or full path; 'none' disables)" },
  { "--apiPlugins",         "-api",         KaString, _vp &apiNames,     KaOpt, _vp NULL,      NULL,  NULL,      "API plugins (comma-separated)" },
  { "--pretty-print",       "-pp",          KaUInt,   _vp &prettySpaces, KaOpt, _vp 0,         _vp 0, _vp 16,   "default JSON indentation (0=compact)" },
  { "--connectionPoolSize", "-cps",         KaInt,    _vp &poolSize,     KaOpt, _vp 32,        _vp 1, _vp 200,  "MHD thread pool size" },
  { "--notifyValueChangeOnly", "-nvco",     KaBool,   _vp &notifyValueChangeOnly, KaOpt, _vp KFALSE, _vp KFALSE, _vp KTRUE, "only notify when an attribute value changed (suppress value-neutral updates)" },
  { "--corsOrigin",         "-corsOrigin",  KaString, _vp &corsOrigin,   KaOpt, _vp NULL,      NULL,  NULL,      "enable CORS with allowed origin ('__ALL' for any)" },
  { "--corsMaxAge",         "-corsMaxAge",  KaInt,    _vp &corsMaxAge,   KaOpt, _vp 86400,     _vp 0, _vp 864000, "preflight cache max age in seconds" },
  { "--defaultUserContext", "-duc",         KaString, _vp &defaultUserContext, KaOpt, _vp NULL, NULL,  NULL,      "default user @context URL" },
  { "--csourceAlias",       "-csourceAlias",KaString, _vp &csourceAlias, KaOpt, _vp NULL,      NULL,  NULL,      "contextSourceAlias base for Via headers (default: the advertised endpoint authority)" },
  { "--httpEndpoint",       "-he",          KaString, _vp &httpEndpoint, KaOpt, _vp NULL,      NULL,  NULL,      "externally-reachable HTTP base URL (default: auto-detected LAN IP, else http://localhost:<port>)" },
  { "--contextSourceExtras","-csx",         KaString, _vp &contextSourceExtras, KaOpt, _vp NULL, NULL, NULL,      "path to a JSON file rendered verbatim on /info/sourceIdentity (§ 5.2.40)" },
  { "--distributed",        "-dist",        KaBool,   _vp &distributed, KaOpt, _vp false, _vp false, _vp true, "enable distributed operations (forward to registered Context Sources); off by default — the Registry API works either way" },
  { "--noSplitEntities",    "-noSplitEntities",KaBool, _vp &noSplitEntities,KaOpt, _vp false, _vp false, _vp true, "disable split entities — each entity fully at one source" },
  { "--high-precision",     "-hp",          KaBool,   _vp &highPrecision, KaOpt, _vp false, _vp false, _vp true, "render DateTime values (createdAt/modifiedAt/observedAt/expiresAt) at full nanosecond precision (9 digits); default is 6 (§5.2.2.4 µs)" },
  { "--asyncSnapshot",      "-asyncSnapshot",  KaBool, _vp &asyncSnapshot, KaOpt, _vp false, _vp false, _vp true, "run snapshotQueries in a background thread (POST returns 201 immediately, status=preparing)" },
  { "--maxRequestSize",     "-mrs",            KaInt,  _vp &maxRequestSize, KaOpt, _vp 2,    _vp 0,    _vp 4096,  "max request body size in MiB (0 = no cap; § 6.3.2 413 threshold)" },
  { "--subStatsFlushInterval","-ssfi",      KaInt,    _vp &subStatsFlushInterval, KaOpt, _vp 60, _vp 0, _vp 86400, "sub-stats periodic flush interval (s; 0 = off)" },
  { "--distOpTimeout",      "-dtmo",        KaInt,    _vp &corRestClientDefaultRequestTimeoutMs, KaOpt, _vp 5000, _vp 1, _vp 600000, "default HTTP client request timeout (ms) — distop forwards, sub-notifs, @context downloads" },
  { "--cooldownMillis",     "-cms",         KaInt,    _vp &cooldownMillis, KaOpt, _vp 30000, _vp 0, _vp 86400000, "default endpoint cooldown after a notification/forward failure (ms; 0 = only when the subscription/registration specifies one)" },
  { "--version",            "-V",           KaBool,   _vp &versionOnly,  KaOpt, _vp KFALSE,    _vp KFALSE, _vp KTRUE, "print version and exit" },
  { "--foreground",         "-fg",          KaBool,   _vp &fg,           KaOpt, _vp KFALSE,    _vp KFALSE, _vp KTRUE, "run in foreground (don't daemonize)" },
  { "--insecureNotif",      "-insecureNotif",KaBool,  _vp &insecureNotif, KaOpt, _vp false, _vp false, _vp true, "accept self-signed certificates on TLS notifications/forwards (endpoint inside a trusted network)" },
  { "--high-availability",  "-ha",          KaString, _vp &haChannel,    KaOpt, _vp NULL,      NULL,  NULL,      "keep the caches in sync with the other broker instances ('mongo' = change streams, needs a replica set; <ip:port> = the haaux server)" },
  KARGS_END
};



// -----------------------------------------------------------------------------
//
// onCrash - print backtrace on segfault
//
static void onCrash(int sigNo)
{
  void*  frames[64];
  int    count = backtrace(frames, 64);
  char** syms  = backtrace_symbols(frames, count);

  fprintf(stderr, "\n=== SIGSEGV backtrace ===\n");
  for (int i = 0; i < count; i++)
    fprintf(stderr, "  %s\n", syms[i]);
  fprintf(stderr, "=========================\n");

  _exit(139);
}



// onSignal -
//
static void onSignal(int sigNo)
{
  (void) sigNo;

  //
  // Stop the periodic loop FIRST. Its dispatch thread calls into the DB plugin
  // (pernot re-queries the entities a periodic subscription watches), so tearing
  // the plugin down underneath it leaves whatever that thread had checked out
  // unreturned - a mongoc client and its guts, ~8.8 KB, which is what the
  // nightly valgrind run reported against subscription_pernot. Worse than the
  // leak: a thread still inside entityQuery would be reading a pool that
  // dbClose has already destroyed.
  //
  // ldPeriodicLoopStop clears the run flag and joins, so on return no other
  // thread can be inside the plugin.
  //
  ldPeriodicLoopStop();

  // Graceful stop: free DB-plugin resources before exit so an in-memory store
  // (corDB) is released rather than leaked — exit(0) then lets valgrind (--vt)
  // and any leak gate see a clean shutdown.
  dbClose();

  exit(0);
}



// -----------------------------------------------------------------------------
//
// pluginsLoad - load DB + API plugins, register their CLI args
//
// Called between kargsInit and kargsParse so that plugin-contributed args
// are known before parsing.
//
static bool pluginsLoad(int argC, char* argV[])
{
  bool startupError = false;

  //
  // Peek at --db and --api before full parse (plugins may contribute args)
  //
  char* dbPeek = kargsPeek(argC, argV, kargV, "--database");
  if (dbPeek == NULL)
    dbPeek = dbName;  // use default

  char* troePeek = kargsPeek(argC, argV, kargV, "--troe");
  if (troePeek == NULL)
    troePeek = troeName;  // use default ("none")

  char* apiPeek = kargsPeek(argC, argV, kargV, "--apiPlugins");

  //
  // Load DB plugin (dlopen + dbRegister, no DB connection yet)
  //
  {
    char errBuf[1024];
    if (pluginLoadDb(dbPeek, errBuf, sizeof(errBuf)) != 0)
    {
      fprintf(stderr, "%s\n", errBuf);
      startupError = true;
    }
    else if (db.args != NULL)
    {
      // Add separator + plugin args to usage
      static char dbSepText[128];
      if (db.alias != NULL)
        snprintf(dbSepText, sizeof(dbSepText), "Database (%s) plugin options:", db.alias);
      else
        snprintf(dbSepText, sizeof(dbSepText), "Database plugin options:");

      static KArg dbSepArgV[] = { KARGS_SEPARATOR(NULL), KARGS_END };
      dbSepArgV[0].description = dbSepText;
      kargsAdd(dbSepArgV);
      kargsAdd(db.args);
    }
  }

  //
  // Load TRoE plugin (dlopen + troeRegister, no connection yet)
  //
  {
    char errBuf[1024];
    if (pluginLoadTroe(troePeek, errBuf, sizeof(errBuf)) != 0)
    {
      fprintf(stderr, "%s\n", errBuf);
      startupError = true;
    }
    else if (troe.args != NULL)
    {
      static char troeSepText[128];
      if (troe.alias != NULL)
        snprintf(troeSepText, sizeof(troeSepText), "TRoE (%s) plugin options:", troe.alias);
      else
        snprintf(troeSepText, sizeof(troeSepText), "TRoE plugin options:");

      static KArg troeSepArgV[] = { KARGS_SEPARATOR(NULL), KARGS_END };
      troeSepArgV[0].description = troeSepText;
      kargsAdd(troeSepArgV);
      kargsAdd(troe.args);
    }
  }

  //
  // Load API plugins (dlopen + apiRegister for each)
  //
  if (apiPeek != NULL)
  {
    char errBuf[1024];
    if (pluginLoadApi(apiPeek, errBuf, sizeof(errBuf)) != 0)
    {
      fprintf(stderr, "%s\n", errBuf);
      startupError = true;
    }
    else
    {
      for (int i = 0; i < apiPluginCount; i++)
      {
        if (apiPlugins[i].args != NULL)
          kargsAdd(apiPlugins[i].args);
      }
    }
  }

  //
  // Add available-plugin info (shown in -u usage output)
  //
  corPluginArgUpdate("--database", "db/currentState");
  corPluginArgUpdate("--troe", "troe/temporal");
  corPluginArgUpdate("--apiPlugins", "api");

  // Add footer showing plugin directory
  static char footerText[256];
  snprintf(footerText, sizeof(footerText), "Plugins are loaded from %s", corPluginBaseDir());
  static KArg footerArgV[] = { KARGS_SEPARATOR(NULL), KARGS_END };
  footerArgV[0].description = footerText;
  kargsAdd(footerArgV);

  return startupError;
}



// -----------------------------------------------------------------------------
//
// apiPluginsInit - register API plugin params and call init()
//
static void apiPluginsInit(void)
{
  for (int i = 0; i < apiPluginCount; i++)
  {
    ApiPlugin* p = &apiPlugins[i];

    if (p->params != NULL)
    {
      if (corRestParamAdd(p->params) == false)
        KT_X(1, "corRestParamAdd failed for API plugin '%s'", p->alias ? p->alias : "?");
    }

    if (p->init != NULL)
    {
      if (p->init() != 0)
        KT_X(1, "init failed for API plugin '%s'", p->alias ? p->alias : "?");
    }
  }
}



// -----------------------------------------------------------------------------
//
// pernotQueryCallback - query entities for a periodic notification subscription
//
// Called by the pernot loop thread. Builds a DbQueryFilter from the
// pernot item's entity selectors and calls db.entityQuery.
//
// db.entityQuery (mongoc, ramdb) allocates through corRest.kjsonP/kalloc.
// corRest is __thread; the pernot thread's copy is zero-initialised so we
// bring it up to working state on first call. Between pernot cycles we
// reset it — the produced entity array is cloned by the caller (through
// ldEntityToApi onto its own kaBuffer) before we return, so nothing
// outside this function holds pointers into corRest afterwards.
//
static KjNode* pernotQueryCallback(void* tenantP, LdPernotItem* itemP, void* allocP)
{
  if (db.entityQuery == NULL)
    return NULL;

  static __thread bool pernotCorRestInited = false;
  if (!pernotCorRestInited)
  {
    kaBufferInit(&corRest.kalloc, corRest.kallocBuffer, sizeof(corRest.kallocBuffer), 256 * 1024, NULL, "pernot");
    corRest.kjsonP = kjBufferCreate(&corRest.kjson, &corRest.kalloc);
    pernotCorRestInited = true;
  }
  else
  {
    //
    // KTRUE = reuse. KFALSE frees the extra blocks and leaves the list that
    // holds them dangling, so the NEXT cycle frees them a second time - a
    // double free that only shows up once a cycle has outgrown the inline
    // buffer twice.
    //
    kaBufferReset(&corRest.kalloc, KTRUE);
  }

  // Build a minimal filter from the pernot item's entity selectors
  DbQueryFilter filter = {0};

  // Extract type from the first entity selector (simplified — full selector
  // support would need to union all types across all selectors)
  if (itemP->entitySelectors != NULL && itemP->entitySelectors->type != NULL)
  {
    static __thread char* typeVBuf[2];
    typeVBuf[0] = itemP->entitySelectors->type;
    typeVBuf[1] = NULL;
    filter.typeV = typeVBuf;
  }

  // Extract id if specific
  if (itemP->entitySelectors != NULL && itemP->entitySelectors->id != NULL)
  {
    static __thread char* idVBuf[2];
    idVBuf[0] = itemP->entitySelectors->id;
    idVBuf[1] = NULL;
    filter.idV = idVBuf;
  }

  filter.qExpr     = itemP->qExpr;
  filter.scopeExpr = itemP->scopeExpr;
  filter.geoRel    = itemP->geoRel;
  filter.limit     = 20;

  KjNode* arrayP = NULL;
  int r = db.entityQuery((Tenant*) tenantP, &filter, &arrayP);

  return (r == DB_OK) ? arrayP : NULL;
}



// -----------------------------------------------------------------------------
//
// throttleRetrieveCallback - § 5.2.x throttling flush: re-query one entity's
// latest LOCAL state by id. Injected into ldThrottleFlushStart so the lib's
// coalesce-to-latest flush can materialize the current state at flush time.
//
// LOCAL view only: whether a notification must ASSEMBLE a distributed/split
// entity is the open spec-doubt #105 — until that resolves, the flush sends the
// triggering broker's local view (a distributed assemble per notification would
// be unaffordable on the write path; here it would be once-per-window, but the
// requirement itself is unsettled). Single-tenant (tenant0) for now, like pernot.
//
static KjNode* throttleRetrieveCallback(const char* entityId, void* allocP)
{
  (void) allocP;
  if (db.entityRetrieve == NULL)
    return NULL;

  KjNode* entityP = NULL;
  if (db.entityRetrieve(&tenant0, entityId, &entityP) != DB_OK)
    return NULL;

  return entityP;
}



// -----------------------------------------------------------------------------
//
// brokerPreServiceHook - chain the tenant + metrics pre-service work
//
// Tenant hook goes first; on its failure we skip the metric bump — a
// request that failed tenant resolution is effectively rejected before
// it hits a service routine.
//
static bool brokerPreServiceHook(void)
{
  // § 6.2.2 Accept-header precondition FIRST — a Not Acceptable media type is a
  // 406 that must trump any other 4xx (e.g. an unacceptable Accept on a retrieve
  // of a missing entity is 406, not 404) and, being a precondition, must reject
  // BEFORE the service routine runs so an unacceptable Accept on a write has no
  // side effect (spec-doubt #109).
  if (!ldAcceptPrecondition())
    return false;

  // Resolve @context unconditionally so service routines see corNgsild.contextP
  // populated whether the request had URL params, a body, or neither
  // (e.g. GET /types/Building with just a Link header).
  ldContextResolve();

  if (!ldUrlWildcardCheck())
    return false;
  if (!tenantPreServiceHook())
    return false;
  if (!ldSnapshotWriteGuard())
    return false;
  metricsPreService();
  return true;
}



// -----------------------------------------------------------------------------
//
// brokerPostResponseHook - metrics first (captures status), then notify dispatch
//
static void brokerPostResponseHook(void)
{
  metricsPostResponse();
  ldNotifyDispatchPending();
  ldCsrSubDispatchPending();
  troeDispatchPending();
  dbExpiredEntityDispatchPending();   // transient Entities a read found expired
  ldRegCacheProbePending();
  ldSubEntityTypeExprsRelease();   // free the per-request subscription type-expr scratch
}



// -----------------------------------------------------------------------------
//
// brokerNotifyStatsHook - called by corNgsild when any notification is POSTed
//
static void brokerNotifyStatsHook(bool csrSub, bool success)
{
  if (csrSub) metricsCsrNotificationSent(success);
  else        metricsNotificationSent(success);
}



// -----------------------------------------------------------------------------
//
// brokerLinkedEntitiesHook - called by corNgsild when notification.join is set
//
static void brokerLinkedEntitiesHook(KjNode* dataArrayP, const char* mode, int joinLevel, bool sysAttrs, void* tenantP)
{
  ldLinkedEntitiesNotifApiArray(dataArrayP, mode, joinLevel, sysAttrs, (Tenant*) tenantP);
}



// -----------------------------------------------------------------------------
//
// contextSourceExtrasLoad - parse the file into ldContextSourceExtras (§ 5.2.40)
//
// `cliPath` is the value of --contextSourceExtras (NULL if not supplied).
// On no CLI override, falls back to the install-time default at
// /opt/seamware/etc/contextSourceExtras.json (regenerated on every build).
//
// Parses with the startup pool (corRest.kalloc) so the raw text and the
// transient parse tree are freed by the pool reset that the caller does
// right after; kjClone(NULL, parsed) deep-copies into malloc-backed
// storage that persists for the broker's lifetime.
//
// CLI override: parse / open / read errors are fatal — misconfiguration
// must surface now, not later via a 500 on /info/sourceIdentity.
// Install-time default: missing file is silently OK (no extras rendered).
//
static void contextSourceExtrasLoad(const char* cliPath)
{
  const char* path        = cliPath;
  bool        cliSupplied = (cliPath != NULL);

  if (path == NULL)
    path = "/opt/seamware/etc/contextSourceExtras.json";

  FILE* fp = fopen(path, "r");
  if (fp == NULL)
  {
    if (cliSupplied)
      KT_X(1, "--contextSourceExtras: cannot open '%s'", path);
    return;
  }

  fseek(fp, 0, SEEK_END);
  long fsz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (fsz <= 0 || fsz > 1024 * 1024)
  {
    fclose(fp);
    KT_X(1, "contextSourceExtras: '%s' empty or too large (max 1 MiB)", path);
  }

  char* buf = (char*) kaAlloc(&corRest.kalloc, fsz + 1);
  if (fread(buf, 1, fsz, fp) != (size_t) fsz)
  {
    fclose(fp);
    KT_X(1, "contextSourceExtras: read failed on '%s'", path);
  }
  fclose(fp);
  buf[fsz] = 0;

  KjNode* parsed = kjParse(corRest.kjsonP, buf);
  if (parsed == NULL)
    KT_X(1, "contextSourceExtras: '%s' is not valid JSON", path);

  ldContextSourceExtras = kjClone(NULL, parsed);
}



// -----------------------------------------------------------------------------
//
// httpEndpointDetect - discover the LAN address peers can actually use to reach us
//
// The forwarded Link header, distributed-subscription callbacks, and served
// @context URLs all embed ldBrokerHttpEndpoint, so it must resolve to an address
// reachable from the OTHER brokers on the network — not "localhost".
//
// The naive route trick (connect a UDP socket to a public IP, read getsockname)
// returns whatever interface owns the default route. On a host running a VPN such
// as CloudflareWARP that is the tunnel address (a /32, POINTOPOINT), which peers on
// the LAN cannot reach. So we scan the interfaces directly and pick the best one:
//
//   hard reject   loopback, admin-down, no-carrier, point-to-point (VPN tun), /32 host addr
//   deprioritize  container/virtual/VPN interface names (docker*, veth*, br-*, tun*, ...)
//   prefer        physical-looking names (en*, eth*, wl*) and RFC-1918 private ranges
//
// The winner becomes the DEFAULT for --httpEndpoint; an explicit --httpEndpoint
// always overrides. Returns true and fills endpoint[] on success.
//
static bool httpEndpointDetect(char* endpoint, size_t endpointLen, unsigned short port)
{
  static const char* virtualPrefix[] =
  {
    "docker", "veth", "br-", "virbr", "vnet", "vmnet", "vbox",
    "tun", "tap", "wg", "tailscale", "zt", "cni", "flannel", "cali", "warp"
  };

  struct ifaddrs* ifList = NULL;
  if (getifaddrs(&ifList) != 0)
    return false;

  char bestIp[INET_ADDRSTRLEN] = { 0 };
  int  bestScore               = -1000000;

  for (struct ifaddrs* ifP = ifList; ifP != NULL; ifP = ifP->ifa_next)
  {
    if (ifP->ifa_addr == NULL)                        continue;
    if (ifP->ifa_addr->sa_family != AF_INET)          continue;   // IPv4 only

    unsigned int flags = ifP->ifa_flags;
    if (flags & IFF_LOOPBACK)                         continue;   // 127.0.0.0/8
    if ((flags & IFF_UP) == 0)                        continue;   // administratively down
    if ((flags & IFF_RUNNING) == 0)                   continue;   // no carrier
    if (flags & IFF_POINTOPOINT)                      continue;   // VPN tunnel (WARP, ppp, ...)

    // A /32 host address is not a usable LAN prefix — another VPN/tunnel tell.
    if (ifP->ifa_netmask != NULL)
    {
      uint32_t mask = ntohl(((struct sockaddr_in*) ifP->ifa_netmask)->sin_addr.s_addr);
      if (mask == 0xffffffff)                         continue;
    }

    struct sockaddr_in* sa = (struct sockaddr_in*) ifP->ifa_addr;
    char                ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip)) == NULL)  continue;

    const char* name  = (ifP->ifa_name != NULL) ? ifP->ifa_name : "";
    int         score = 0;

    for (unsigned int i = 0; i < sizeof(virtualPrefix) / sizeof(virtualPrefix[0]); i++)
    {
      if (strncmp(name, virtualPrefix[i], strlen(virtualPrefix[i])) == 0)
      {
        score -= 1000;   // last resort only
        break;
      }
    }

    if ((strncmp(name, "en", 2) == 0) || (strncmp(name, "eth", 3) == 0) ||
        (strncmp(name, "wl", 2) == 0) || (strncmp(name, "em", 2) == 0))
      score += 100;      // physical-looking name

    uint32_t a = ntohl(sa->sin_addr.s_addr);
    if (((a & 0xff000000) == 0x0a000000) ||           // 10.0.0.0/8
        ((a & 0xfff00000) == 0xac100000) ||           // 172.16.0.0/12
        ((a & 0xffff0000) == 0xc0a80000))             // 192.168.0.0/16
      score += 10;       // RFC-1918 private — a LAN broker's reachable address

    if (score > bestScore)
    {
      bestScore = score;
      strncpy(bestIp, ip, sizeof(bestIp) - 1);
      bestIp[sizeof(bestIp) - 1] = 0;
    }
  }

  freeifaddrs(ifList);

  if (bestIp[0] == 0)
    return false;

  snprintf(endpoint, endpointLen, "http://%s:%u", bestIp, (unsigned) port);
  return true;
}



// -----------------------------------------------------------------------------
//
// main -
//
int main(int argC, char* argV[])
{
  // Strip path from program name (use basename only)
  char* progName = strrchr(argV[0], '/');
  progName = (progName != NULL) ? progName + 1 : argV[0];

  //
  // --version is answered before kargs is initialized and before any plugin is
  // loaded: asking a binary what it is must not depend on a plugin directory
  // being present, nor on a DB plugin being loadable. It is the one question a
  // broken installation still has to be able to answer.
  //
  // kargsPeek is what makes that possible without hand-parsing argV - it reads
  // the option table directly, answers "SET" for a KaBool, and matches the short
  // name too. It is the same mechanism the plugin peek below uses.
  //
  if (kargsPeek(argC, argV, kargV, "--version") != NULL)
  {
    printf("%s %s\n", progName, CORAINE_VERSION);
    exit(0);
  }

  KArgsStatus ks = kargsInit(progName, kargV, "CORAINE");
  if (ks != KargsOk)
    KT_X(1, "kargsInit failed: %s", kargsStatus(ks));

  corPluginSetBaseDir("/opt/seamware/plugins", "SEAMWARE_PLUGIN_DIR");

  bool startupError = pluginsLoad(argC, argV);

  ks = kargsParse(argC, argV);
  if (ks != KargsOk)
    KT_X(1, "kargsParse failed: %s", kargsStatus(ks));

  if (startupError)
  {
    kargsUsage();
    exit(1);
  }

  ldNotifyValueChangeOnly = notifyValueChangeOnly;
  ldSplitEntities       = !noSplitEntities;  // default: true (split entities is standard NGSI-LD behavior)
  ldDistributed         = distributed;      // default: false — every entity operation stays local until asked otherwise
  ldTimestampHighPrecision = highPrecision;  // default false → 6 fractional digits (§5.2.2.4); -hp → 9
  ldDefaultContextUrl   = defaultUserContext;
  ldBrokerStartTimeSec  = (long long) time(NULL);

  //
  // Externally-reachable HTTP base URL — embedded in forwarded Link headers,
  // the callback root of derived (distributed) subscriptions (§ 5.8.1.4), and
  // served @context URLs. Peers on other hosts must be able to reach it, so the
  // default is auto-discovered from the LAN interfaces (httpEndpointDetect);
  // --httpEndpoint overrides, and http://localhost:<port> is the last-resort
  // fallback when no usable interface is found.
  //
  const char* endpointSource;
  if (httpEndpoint != NULL)
  {
    ldBrokerHttpEndpoint = httpEndpoint;
    endpointSource       = "--httpEndpoint";
  }
  else
  {
    static char defaultEndpoint[64];
    if (httpEndpointDetect(defaultEndpoint, sizeof(defaultEndpoint), port))
      endpointSource = "auto-detected LAN IP";
    else
    {
      snprintf(defaultEndpoint, sizeof(defaultEndpoint), "http://localhost:%u", (unsigned) port);
      endpointSource = "fallback (no LAN interface found)";
    }
    ldBrokerHttpEndpoint = defaultEndpoint;
  }

  //
  // contextSourceAlias base for Via headers and /info/sourceIdentity
  // (NGSI-LD § 5.7.5, § 9.7). It has to be UNIQUE PER BROKER: loop detection
  // treats a registration whose probed alias equals ours as pointing back at
  // us, and drops the forward — silently, for an inclusive registration.
  //
  // It is therefore derived from the broker's own advertised endpoint, whose
  // authority (host[:port]) is exactly "who I am on the network": an explicit
  // --httpEndpoint when given, else the auto-detected LAN address.
  //
  // The old default was "<argv0-basename>:<port>", which is unique only when
  // brokers differ by PORT. Two brokers on separate hosts or containers both
  // listening on 1026 — the ordinary deployment — both called themselves
  // "coraine:1026", so neither would forward to the other. Ports differing is
  // exactly the test topology, which is why no test ever caught it.
  //
  // --csourceAlias still wins, and the basename form remains the last resort.
  //
  if (csourceAlias != NULL)
    ldCsourceAliasBase = csourceAlias;
  else
  {
    static char defaultAlias[128];
    const char* authority = strstr(ldBrokerHttpEndpoint, "://");

    authority = (authority != NULL) ? authority + 3 : ldBrokerHttpEndpoint;

    if (authority[0] != 0)
    {
      const char* slash = strchr(authority, '/');
      int         len   = (slash != NULL) ? (int) (slash - authority) : (int) strlen(authority);

      if (len > (int) sizeof(defaultAlias) - 1)
        len = sizeof(defaultAlias) - 1;

      memcpy(defaultAlias, authority, len);
      defaultAlias[len] = 0;
    }
    else
    {
      const char* basename = argV[0];
      for (const char* p = argV[0]; *p != 0; p++)
        if (*p == '/') basename = p + 1;

      snprintf(defaultAlias, sizeof(defaultAlias), "%s:%u", basename, (unsigned) port);
    }

    ldCsourceAliasBase = defaultAlias;
  }


  int r = ktInit("coraine", NULL, true, NULL, traceLevels, kaBuiltinVerbose, kaBuiltinDebug, false);
  if (r != 0)
    KT_X(1, "ktInit failed");

  KT_V("coraine  %s", CORAINE_VERSION);
  KT_I("Advertised HTTP endpoint: %s (%s)", ldBrokerHttpEndpoint, endpointSource);

  signal(SIGINT,  onSignal);
  signal(SIGTERM, onSignal);
  signal(SIGSEGV, onCrash);

  // User-Agent uses the <product>/<version> form — a bare
  // product name (e.g. "coraine") is blocked by some @context CDNs
  // (uri.etsi.org via Cloudflare) but slash-versioned tokens pass.
  if (corRestClientInit(4, 60, "Coraine/" CORAINE_VERSION) != 0)
    KT_X(1, "corRestClientInit failed");

  // --insecureNotif → notifications/forwards to TLS endpoints accept self-signed
  // certificates (set before the first TLS handshake, i.e. before corRestClientTlsInit)
  if (insecureNotif)
  {
    corRestClientTlsInsecureSet(true);
    ldMqttTlsInsecureSet(true);   // same for mqtts:// notification endpoints
  }

  static KAlloc  contextAlloc;
  static char    contextBuffer[64 * 1024];

  // allocSize must be non-zero — kaAlloc falls back to calloc(1, allocSize)
  // when the static 64 KiB initial buffer runs out, and calloc(1, 0) returns
  // NULL/empty, leading to a SEGV on the next memset. 256 KiB matches the
  // pernot/corRest convention for "ample headroom for normal growth".
  kaBufferInit(&contextAlloc, contextBuffer, sizeof(contextBuffer), 256 * 1024, NULL, "jsonld-context");

  if (corLdInit(&contextAlloc, NULL, contextDownload, contextError) != 0)
    KT_X(1, "corLdInit failed");

  if (ldInit() != 0)
    KT_X(1, "ldInit failed");

  ldDefaultCooldownNs = (uint64_t) cooldownMillis * 1000000ULL;

  forwardingHttpRegister();

  if (prettySpaces > 0)
    corRestSetPrettySpaces(prettySpaces);

  // § 6.3.2 413 threshold. 0 in --maxRequestSize disables the cap.
  corRestSetMaxRequestSize(((unsigned long long) maxRequestSize) * 1024ULL * 1024ULL);

  if (corsOrigin != NULL)
  {
    const char* origin = (strcmp(corsOrigin, "__ALL") == 0) ? "*" : corsOrigin;

    CorRestCorsConfig corsConf = {
      .allowOrigin   = origin,
      .allowHeaders  = "Content-Type, Accept, Link, NGSILD-Tenant, NGSILD-Path, Authorization",
      .exposeHeaders = "Location, NGSILD-Results-Count, Link, NGSILD-Tenant, NGSILD-Warning",
      .maxAge        = corsMaxAge
    };
    corRestCorsConfig(&corsConf);
  }

  apiPluginsInit();
  tenantInit("cor");
  metricsInit();
  ldNotifyStatsHookSet(brokerNotifyStatsHook);
  ldLinkedEntitiesHookSet(brokerLinkedEntitiesHook);
  corRestSetServiceInitHook(ldUrlWildcardOptionsInit);
  corRestSetPreServiceHook(brokerPreServiceHook);
  corRestSetPostResponseHook(brokerPostResponseHook);

  if (dbStart() != 0)
    KT_X(1, "dbStart failed");

  if (troeStart() != 0)
    KT_X(1, "troeStart failed");

  //
  // Load subscriptions from DB into cache.
  // mongoc uses corRest.kalloc internally — set up a startup buffer for it.
  // Cache items get cloned into persistent (malloc) storage, so this is short-lived.
  //
  static char startupKallocBuf[16384];
  kaBufferInit(&corRest.kalloc, startupKallocBuf, sizeof(startupKallocBuf), 4096, NULL, "startup");
  corRest.kjsonP = kjBufferCreate(&corRest.kjson, &corRest.kalloc);
  //
  // --ha: from here on, what another broker instance writes reaches our caches
  // too. BEFORE the reload below, not after - see haInit.h: listening only after
  // reading the database leaves a window whose changes are missed for good.
  // Nothing is applied until haApplyEnable() below.
  //
  if (haInit() == false)
    KT_X(1, "unable to start the HA channel '%s'", haChannel);

  tenantSubCacheReload();
  tenantRegCacheReload();
  tenantSnapshotCacheReload();
  contextCacheReload();

  haApplyEnable();

  contextSourceExtrasLoad(contextSourceExtras);

  kaBufferReset(&corRest.kalloc, false);

  // Register the pernot subsystem with the shared periodic-dispatch
  // engine. The engine itself is launched once below.
  if (tenant0.pernotCacheP != NULL)
    ldPernotLoopStart((LdPernotCache*) tenant0.pernotCacheP, pernotQueryCallback);

  // § 5.2.x throttling — register the coalesce-to-latest flush (sole sender for
  // throttled subs; the synchronous path only buffers into the dirty set).
  if (tenant0.subCacheP != NULL)
    ldThrottleFlushStart((LdSubCache*) tenant0.subCacheP, throttleRetrieveCallback);

  // § 5.11.7 — register the CSR-Sub periodic ticker. Skips items with
  // timeInterval == 0 (change-driven) by design.
  if (tenant0.regSubCacheP != NULL && tenant0.regCacheP != NULL)
    ldCsrSubPeriodicLoopRegister((LdSubCache*) tenant0.regSubCacheP,
                                  (LdRegCache*) tenant0.regCacheP);

  // Register the volatile-context reaper — drops never-fetched one-shot
  // hosted contexts (response / forward Link targets) past their TTL.
  ldContextHostReaperStart();

  // Start the shared periodic dispatch thread (1-Hz tick over all
  // registered consumers).
  ldPeriodicLoopStart();

  // Start the sub-stats periodic flush loop. --subStatsFlushInterval
  // defaults to 60s; 0 disables. The admin endpoint remains available
  // regardless.
  ldStatsFlushLoopStart(subStatsFlushInterval, subStatsFlushAll);

  //
  // Build combined service array (core + plugins) and start the REST server
  //
  int totalServices = 0;
  CorRestServiceSimplified* allServices = serviceBuild(&totalServices);
  if (allServices == NULL)
    KT_X(1, "serviceBuild failed (out of memory)");

  if (corRestInit(allServices, totalServices, (unsigned short) port, poolSize) != 0)
    KT_X(1, "corRestInit failed on port %u", port);

  KT_I("coraine running on port %u", port);

  pause();

  return 0;
}

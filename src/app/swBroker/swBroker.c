//
// FILE            swBroker.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdbool.h>                              // bool, true, false
#include <stdio.h>                                // snprintf, fprintf
#include <stdlib.h>                               // _exit
#include <unistd.h>                               // pause
#include <signal.h>                               // signal, SIGINT, SIGTERM
#include <string.h>                               // strcmp, memcpy
#include <time.h>                                 // time
#include <execinfo.h>                             // backtrace, backtrace_symbols

#include "kalloc/kalloc.h"                        // KAlloc, kaBufferInit
#include "ktrace/kTrace.h"                        // KT_I, KT_V, KT_X
#include "kargs/kargs.h"                          // kargsInit, kargsParse, kargsPeek, KArg, KArgsStatus, kargsStatus, KARGS_END, kargsUsage
#include "swPlugin/swPlugin.h"                    // swPluginSetBaseDir, swPluginBaseDir, swPluginArgUpdate
#include "swRest/swRest.h"                        // swRestInit, swRestSetPrettySpaces, swRestSetPreServiceHook, swRestParamAdd
#include "swRest/swRestClient.h"                  // swRestClientInit, SwRestClientRequest/Response
#include "swJsonld/swJsonld.h"                    // swldInit, SWJSONLD_VERSION
#include "swJsonld/SwldContext.h"                 // SwldContext, SwldContextKind
#include "swJsonld/SwldContextCache.h"            // SwldContextCache
#include "swJsonld/swldCache.h"                   // swldCacheInsert
#include "swJsonld/swldContextParse.h"            // swldContextFromObject

#include "kjson/kjson.h"                          // Kjson
#include "kjson/kjBufferCreate.h"                 // kjBufferCreate
#include "kjson/kjParse.h"                        // kjParse
#include "kjson/kjLookup.h"                       // kjLookup
#include "kjson/kjClone.h"                        // kjClone
#include "kalloc/kaAlloc.h"                       // kaAlloc
#include "kalloc/kaStrdup.h"                      // kaStrdup
#include "swNgsild/swNgsild.h"                    // ldInit, ldLocalOnly, SWNGSILD_VERSION, ldParamsInit
#include "swNgsild/ldUrlWildcardCheck.h"          // ldUrlWildcardCheck
#include "swNgsild/ldHooks.h"                      // ldAcceptPrecondition
#include "swNgsild/ldNotifyDefer.h"               // ldNotifyDispatchPending
#include "swNgsild/ldRegCache.h"                  // ldRegCacheProbePending
#include "swNgsild/ldNotifyStatsHook.h"           // ldNotifyStatsHookSet
#include "swNgsild/ldLinkedEntitiesHook.h"        // ldLinkedEntitiesHookSet
#include "linkedEntities/ldLinkedEntities.h"      // ldLinkedEntitiesNotifApiArray
#include "swNgsild/LdPernotCache.h"               // LdPernotCache, LdPernotItem
#include "swNgsild/ldPernotLoop.h"                // ldPernotLoopStart
#include "swNgsild/ldPeriodicLoop.h"              // ldPeriodicLoopStart, ldPeriodicLoopStop
#include "swNgsild/ldContextHost.h"               // ldContextHostReaperStart
#include "swNgsild/ldCsrSubNotify.h"              // ldCsrSubPeriodicLoopRegister, ldCsrSubDispatchPending
#include "swNgsild/ldCheckSubscription.h"         // ldSubEntityTypeExprsRelease
#include "swNgsild/ldStatsFlushLoop.h"            // ldStatsFlushLoopStart
#include "swNgsild/ldMqttNotify.h"                // ldMqttTlsInsecureSet
#include "metrics/subStatsFlushAll.h"             // subStatsFlushAll
#include "swNgsild/SwNgsild.h"                    // swNgsild, ldCsourceAliasBase

#include "db/DbDriver.h"                          // db, DB_OK
#include "db/DbQueryFilter.h"                     // DbQueryFilter
#include "db/dbInit.h"                            // dbStart
#include "db/dbClose.h"                           // dbClose
#include "db/Tenant.h"                            // tenantPreServiceHook
#include "serviceRoutines/ldSnapshotRead.h"       // ldSnapshotWriteGuard

#include "troe/TroeDriver.h"                      // troe
#include "troe/troeInit.h"                        // troeStart, troeStop
#include "troe/troeDispatch.h"                    // troeDispatchPending

#include "plugin/ApiPlugin.h"                     // ApiPlugin, apiPlugins, apiPluginCount
#include "plugin/pluginLoader.h"                  // pluginLoadDb, pluginLoadApi

#include "forwarding/forwardingHttp.h"            // forwardingHttpRegister

#include "metrics/metrics.h"                      // metricsInit, metricsPreService, metricsPostResponse, metricsNotificationSent, metricsCsrNotificationSent

#include "ngsildServices.h"                       // ngsildCoreServices, serviceBuild



// -----------------------------------------------------------------------------
//
// contextDownload - SwldDownloadFunction callback for fetching remote @contexts
//
static char* contextDownload(const char* url, int* statusCodeP)
{
  SwRestClientRequest  req;
  SwRestClientResponse resp;

  swRestClientRequestInit(&req, SwVerbGet, url, NULL);
  swRestClientRequestHeader(&req, "Accept", "application/ld+json, application/json");
  swRestClientRequestTimeout(&req, 5000, 10000);

  int r = swRestClientSend(&req, &resp);

  if (r != SWC_OK || resp.statusCode != 200)
  {
    *statusCodeP = (resp.statusCode > 0) ? resp.statusCode : 500;
    swRestClientResponseCleanup(&resp);
    return NULL;
  }

  *statusCodeP = 200;

  // Return a malloc'd copy of the body (swJsonld will free it)
  char* copy = NULL;
  if (resp.body != NULL && resp.bodyLen > 0)
  {
    copy = (char*) malloc(resp.bodyLen + 1);
    memcpy(copy, resp.body, resp.bodyLen);
    copy[resp.bodyLen] = 0;
  }

  swRestClientResponseCleanup(&resp);
  return copy;
}



// -----------------------------------------------------------------------------
//
// swldCacheGet - internal accessor in swJsonld/swldInit.c (cache allocator)
//
extern SwldContextCache* swldCacheGet(void);



// -----------------------------------------------------------------------------
//
// contextCacheReload - re-populate the JSON-LD cache from the persisted
// "swBroker" database. Called once at startup, after dbStart().
//
static void contextCacheReload(void)
{
  if (db.contextList == NULL)
    return;

  KAlloc* storeP = swldCacheGet()->kaP;

  DbContextRow* rows  = NULL;
  int           count = 0;

  if (db.contextList(storeP, &rows, &count) != DB_OK)
    return;

  for (int i = 0; i < count; i++)
  {
    DbContextRow* r = &rows[i];

    if (r->id == NULL || r->body == NULL)
      continue;

    //
    // Parse the body (a stand-alone JSON-LD context document) into a tree
    // and pull out @context. We use the cache allocator so the resulting
    // SwldContext outlives this call.
    //
    char*  bodyForParse = kaStrdup(storeP, r->body);  // kjParse is destructive
    Kjson  kjson;
    Kjson* kjsonP = kjBufferCreate(&kjson, storeP);

    KjNode* treeP = kjParse(kjsonP, bodyForParse);
    if (treeP == NULL)
      continue;

    KjNode* atContextP = kjLookup(treeP, "@context");
    if (atContextP == NULL || atContextP->type != KjObject)
      continue;

    SwldContext* contextP = swldContextFromObject(atContextP, storeP, r->url);
    if (contextP == NULL)
      continue;

    contextP->id   = kaStrdup(storeP, r->id);
    contextP->body = kaStrdup(storeP, r->body);
    contextP->kind = (r->kind == DB_CONTEXT_KIND_HOSTED) ? SwldKindHosted : SwldKindCached;

    swldCacheInsert(contextP);
  }
}



#include "swBrokerVersion.h"                      // SWBROKER_VERSION



// -----------------------------------------------------------------------------
//
// Command line arguments
//
unsigned short port         = 1026;
char*          dbName       = "mongoc";
char*          troeName     = "none";
char*          apiNames     = NULL;
unsigned int   prettySpaces = 0;
bool           localOnly    = false;
bool           notifyValueChangeOnly = false;
bool           fg           = false;
int            poolSize     = 32;
char*          corsOrigin   = NULL;
int            corsMaxAge   = 86400;
char*          defaultUserContext  = NULL;
char*          csourceAlias = NULL;
char*          httpEndpoint = NULL;
char*          contextSourceExtras = NULL;  // path to a JSON file (§ 5.2.40)
char*          traceLevels  = NULL;
bool           noSplitEntities = false;
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
  { "--localOnly",          "-local",       KaBool,   _vp &localOnly,    KaOpt, _vp KFALSE,    _vp KFALSE, _vp KTRUE, "local-only mode (no distributed operations)" },
  { "--notifyValueChangeOnly", "-nvco",     KaBool,   _vp &notifyValueChangeOnly, KaOpt, _vp KFALSE, _vp KFALSE, _vp KTRUE, "only notify when an attribute value changed (suppress value-neutral updates)" },
  { "--corsOrigin",         "-corsOrigin",  KaString, _vp &corsOrigin,   KaOpt, _vp NULL,      NULL,  NULL,      "enable CORS with allowed origin ('__ALL' for any)" },
  { "--corsMaxAge",         "-corsMaxAge",  KaInt,    _vp &corsMaxAge,   KaOpt, _vp 86400,     _vp 0, _vp 864000, "preflight cache max age in seconds" },
  { "--defaultUserContext", "-duc",         KaString, _vp &defaultUserContext, KaOpt, _vp NULL, NULL,  NULL,      "default user @context URL" },
  { "--csourceAlias",       "-csourceAlias",KaString, _vp &csourceAlias, KaOpt, _vp NULL,      NULL,  NULL,      "contextSourceAlias base for Via headers (default: <exe>:<port>)" },
  { "--httpEndpoint",       "-he",          KaString, _vp &httpEndpoint, KaOpt, _vp NULL,      NULL,  NULL,      "externally-reachable HTTP base URL (default: http://localhost:<port>)" },
  { "--contextSourceExtras","-csx",         KaString, _vp &contextSourceExtras, KaOpt, _vp NULL, NULL, NULL,      "path to a JSON file rendered verbatim on /info/sourceIdentity (§ 5.2.40)" },
  { "--noSplitEntities",    "-noSplitEntities",KaBool, _vp &noSplitEntities,KaOpt, _vp false, _vp false, _vp true, "disable split entities — each entity fully at one source" },
  { "--high-precision",     "-hp",          KaBool,   _vp &highPrecision, KaOpt, _vp false, _vp false, _vp true, "render sysattr timestamps with full nanosecond precision (9 digits); default is 6 (§5.2.2.4 µs)" },
  { "--asyncSnapshot",      "-asyncSnapshot",  KaBool, _vp &asyncSnapshot, KaOpt, _vp false, _vp false, _vp true, "run snapshotQueries in a background thread (POST returns 201 immediately, status=preparing)" },
  { "--maxRequestSize",     "-mrs",            KaInt,  _vp &maxRequestSize, KaOpt, _vp 2,    _vp 0,    _vp 4096,  "max request body size in MiB (0 = no cap; § 6.3.2 413 threshold)" },
  { "--subStatsFlushInterval","-ssfi",      KaInt,    _vp &subStatsFlushInterval, KaOpt, _vp 60, _vp 0, _vp 86400, "sub-stats periodic flush interval (s; 0 = off)" },
  { "--distOpTimeout",      "-dtmo",        KaInt,    _vp &swRestClientDefaultRequestTimeoutMs, KaOpt, _vp 5000, _vp 1, _vp 600000, "default HTTP client request timeout (ms) — distop forwards, sub-notifs, @context downloads" },
  { "--cooldownMillis",     "-cms",         KaInt,    _vp &cooldownMillis, KaOpt, _vp 30000, _vp 0, _vp 86400000, "default endpoint cooldown after a notification/forward failure (ms; 0 = only when the subscription/registration specifies one)" },
  { "--foreground",         "-fg",          KaBool,   _vp &fg,           KaOpt, _vp KFALSE,    _vp KFALSE, _vp KTRUE, "run in foreground (don't daemonize)" },
  { "--insecureNotif",      "-insecureNotif",KaBool,  _vp &insecureNotif, KaOpt, _vp false, _vp false, _vp true, "accept self-signed certificates on TLS notifications/forwards (endpoint inside a trusted network)" },
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

  // Graceful stop: free DB-plugin resources before exit so an in-memory store
  // (swRamDB) is released rather than leaked — exit(0) then lets valgrind (--vt)
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
  swPluginArgUpdate("--database", "db/currentState");
  swPluginArgUpdate("--troe", "troe/temporal");
  swPluginArgUpdate("--apiPlugins", "api");

  // Add footer showing plugin directory
  static char footerText[256];
  snprintf(footerText, sizeof(footerText), "Plugins are loaded from %s", swPluginBaseDir());
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
      if (swRestParamAdd(p->params) == false)
        KT_X(1, "swRestParamAdd failed for API plugin '%s'", p->alias ? p->alias : "?");
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
// db.entityQuery (mongoc, ramdb) allocates through swRest.kjsonP/kalloc.
// swRest is __thread; the pernot thread's copy is zero-initialised so we
// bring it up to working state on first call. Between pernot cycles we
// reset it — the produced entity array is cloned by the caller (through
// ldEntityToApi onto its own kaBuffer) before we return, so nothing
// outside this function holds pointers into swRest afterwards.
//
static KjNode* pernotQueryCallback(void* tenantP, LdPernotItem* itemP, void* allocP)
{
  if (db.entityQuery == NULL)
    return NULL;

  static __thread bool pernotSwRestInited = false;
  if (!pernotSwRestInited)
  {
    kaBufferInit(&swRest.kalloc, swRest.kallocBuffer, sizeof(swRest.kallocBuffer), 256 * 1024, NULL, "pernot");
    swRest.kjsonP = kjBufferCreate(&swRest.kjson, &swRest.kalloc);
    pernotSwRestInited = true;
  }
  else
  {
    kaBufferReset(&swRest.kalloc, KFALSE);
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

  // Resolve @context unconditionally so service routines see swNgsild.contextP
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
  ldRegCacheProbePending();
  ldSubEntityTypeExprsRelease();   // free the per-request subscription type-expr scratch
}



// -----------------------------------------------------------------------------
//
// brokerNotifyStatsHook - called by swNgsild when any notification is POSTed
//
static void brokerNotifyStatsHook(bool csrSub, bool success)
{
  if (csrSub) metricsCsrNotificationSent(success);
  else        metricsNotificationSent(success);
}



// -----------------------------------------------------------------------------
//
// brokerLinkedEntitiesHook - called by swNgsild when notification.join is set
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
// Parses with the startup pool (swRest.kalloc) so the raw text and the
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

  char* buf = (char*) kaAlloc(&swRest.kalloc, fsz + 1);
  if (fread(buf, 1, fsz, fp) != (size_t) fsz)
  {
    fclose(fp);
    KT_X(1, "contextSourceExtras: read failed on '%s'", path);
  }
  fclose(fp);
  buf[fsz] = 0;

  KjNode* parsed = kjParse(swRest.kjsonP, buf);
  if (parsed == NULL)
    KT_X(1, "contextSourceExtras: '%s' is not valid JSON", path);

  ldContextSourceExtras = kjClone(NULL, parsed);
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

  KArgsStatus ks = kargsInit(progName, kargV, "SWBROKER");
  if (ks != KargsOk)
    KT_X(1, "kargsInit failed: %s", kargsStatus(ks));

  swPluginSetBaseDir("/opt/seamware/plugins", "SEAMWARE_PLUGIN_DIR");

  bool startupError = pluginsLoad(argC, argV);

  ks = kargsParse(argC, argV);
  if (ks != KargsOk)
    KT_X(1, "kargsParse failed: %s", kargsStatus(ks));

  if (startupError)
  {
    kargsUsage();
    exit(1);
  }

  ldLocalOnly           = localOnly;
  ldNotifyValueChangeOnly = notifyValueChangeOnly;
  ldSplitEntities       = !noSplitEntities;  // default: true (split entities is standard NGSI-LD behavior)
  ldTimestampHighPrecision = highPrecision;  // default false → 6 fractional digits (§5.2.2.4); -hp → 9
  ldDefaultContextUrl   = defaultUserContext;
  ldBrokerStartTimeSec  = (long long) time(NULL);

  //
  // contextSourceAlias base for Via headers (NGSI-LD § 5.7.5).
  // Default: "<argv0-basename>:<port>" — RFC 7230 pseudonym is 1*VCHAR,
  // no hostname requirement, so the executable name is sufficient and
  // sidesteps the multi-IP host-naming problem.
  //
  if (csourceAlias == NULL)
  {
    const char* basename = argV[0];
    for (const char* p = argV[0]; *p != 0; p++)
      if (*p == '/') basename = p + 1;

    static char defaultAlias[128];
    snprintf(defaultAlias, sizeof(defaultAlias), "%s:%u", basename, (unsigned) port);
    ldCsourceAliasBase = defaultAlias;
  }
  else
    ldCsourceAliasBase = csourceAlias;

  //
  // Externally-reachable HTTP base URL — used as the callback root in
  // notification.endpoint.uri of derived (distributed) subscriptions
  // (§ 5.8.1.4). Defaults to http://localhost:<port> for tests; real
  // multi-host setups override via --httpEndpoint.
  //
  if (httpEndpoint != NULL)
    ldBrokerHttpEndpoint = httpEndpoint;
  else
  {
    static char defaultEndpoint[64];
    snprintf(defaultEndpoint, sizeof(defaultEndpoint), "http://localhost:%u", (unsigned) port);
    ldBrokerHttpEndpoint = defaultEndpoint;
  }


  int r = ktInit("swBroker", NULL, true, NULL, traceLevels, kaBuiltinVerbose, kaBuiltinDebug, false);
  if (r != 0)
    KT_X(1, "ktInit failed");

  KT_V("swBroker  %s", SWBROKER_VERSION);

  signal(SIGINT,  onSignal);
  signal(SIGTERM, onSignal);
  signal(SIGSEGV, onCrash);

  // User-Agent format follows the orionld/<version> convention — bare
  // product names (e.g. "swBroker") are blocked by some @context CDNs
  // (uri.etsi.org via Cloudflare) but slash-versioned tokens pass.
  if (swRestClientInit(4, 60, "swBroker/" SWBROKER_VERSION) != 0)
    KT_X(1, "swRestClientInit failed");

  // --insecureNotif → notifications/forwards to TLS endpoints accept self-signed
  // certificates (set before the first TLS handshake, i.e. before swRestClientTlsInit)
  if (insecureNotif)
  {
    swRestClientTlsInsecureSet(true);
    ldMqttTlsInsecureSet(true);   // same for mqtts:// notification endpoints
  }

  static KAlloc  contextAlloc;
  static char    contextBuffer[64 * 1024];

  // allocSize must be non-zero — kaAlloc falls back to calloc(1, allocSize)
  // when the static 64 KiB initial buffer runs out, and calloc(1, 0) returns
  // NULL/empty, leading to a SEGV on the next memset. 256 KiB matches the
  // pernot/swRest convention for "ample headroom for normal growth".
  kaBufferInit(&contextAlloc, contextBuffer, sizeof(contextBuffer), 256 * 1024, NULL, "jsonld-context");

  if (swldInit(&contextAlloc, NULL, contextDownload) != 0)
    KT_X(1, "swldInit failed");

  if (ldInit() != 0)
    KT_X(1, "ldInit failed");

  ldDefaultCooldownNs = (uint64_t) cooldownMillis * 1000000ULL;

  forwardingHttpRegister();

  if (prettySpaces > 0)
    swRestSetPrettySpaces(prettySpaces);

  // § 6.3.2 413 threshold. 0 in --maxRequestSize disables the cap.
  swRestSetMaxRequestSize(((unsigned long long) maxRequestSize) * 1024ULL * 1024ULL);

  if (corsOrigin != NULL)
  {
    const char* origin = (strcmp(corsOrigin, "__ALL") == 0) ? "*" : corsOrigin;

    SwRestCorsConfig corsConf = {
      .allowOrigin   = origin,
      .allowHeaders  = "Content-Type, Accept, Link, NGSILD-Tenant, NGSILD-Path, Authorization",
      .exposeHeaders = "Location, NGSILD-Results-Count, Link, NGSILD-Tenant, NGSILD-Warning",
      .maxAge        = corsMaxAge
    };
    swRestCorsConfig(&corsConf);
  }

  apiPluginsInit();
  tenantInit("sw");
  metricsInit();
  ldNotifyStatsHookSet(brokerNotifyStatsHook);
  ldLinkedEntitiesHookSet(brokerLinkedEntitiesHook);
  swRestSetServiceInitHook(ldUrlWildcardOptionsInit);
  swRestSetPreServiceHook(brokerPreServiceHook);
  swRestSetPostResponseHook(brokerPostResponseHook);

  if (dbStart() != 0)
    KT_X(1, "dbStart failed");

  if (troeStart() != 0)
    KT_X(1, "troeStart failed");

  //
  // Load subscriptions from DB into cache.
  // mongoc uses swRest.kalloc internally — set up a startup buffer for it.
  // Cache items get cloned into persistent (malloc) storage, so this is short-lived.
  //
  static char startupKallocBuf[16384];
  kaBufferInit(&swRest.kalloc, startupKallocBuf, sizeof(startupKallocBuf), 4096, NULL, "startup");
  swRest.kjsonP = kjBufferCreate(&swRest.kjson, &swRest.kalloc);
  tenantSubCacheReload();
  tenantRegCacheReload();
  tenantSnapshotCacheReload();
  contextCacheReload();

  contextSourceExtrasLoad(contextSourceExtras);

  kaBufferReset(&swRest.kalloc, false);

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
  SwRestServiceSimplified* allServices = serviceBuild(&totalServices);
  if (allServices == NULL)
    KT_X(1, "serviceBuild failed (out of memory)");

  if (swRestInit(allServices, totalServices, (unsigned short) port, poolSize) != 0)
    KT_X(1, "swRestInit failed on port %u", port);

  KT_I("swBroker running on port %u", port);

  pause();

  return 0;
}

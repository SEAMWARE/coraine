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
#include "kalloc/kaStrdup.h"                      // kaStrdup
#include "swNgsild/swNgsild.h"                    // ldInit, ldLocalOnly, SWNGSILD_VERSION, ldParamsInit
#include "swNgsild/ldNotifyDefer.h"               // ldNotifyDispatchPending
#include "swNgsild/ldNotifyStatsHook.h"           // ldNotifyStatsHookSet
#include "swNgsild/LdPernotCache.h"               // LdPernotCache, LdPernotItem
#include "swNgsild/ldPernotLoop.h"                // ldPernotLoopStart
#include "swNgsild/SwNgsild.h"                    // swNgsild, ldCsourceAliasBase

#include "db/DbDriver.h"                          // db, DB_OK
#include "db/DbQueryFilter.h"                     // DbQueryFilter
#include "db/dbInit.h"                            // dbStart
#include "db/dbClose.h"                           // dbClose
#include "db/Tenant.h"                            // tenantPreServiceHook

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
    return NULL;
  }

  *statusCodeP = 200;

  // Return a malloc'd copy of the body (swJsonld will free it)
  if (resp.body != NULL && resp.bodyLen > 0)
  {
    char* copy = (char*) malloc(resp.bodyLen + 1);
    memcpy(copy, resp.body, resp.bodyLen);
    copy[resp.bodyLen] = 0;
    return copy;
  }

  return NULL;
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



// -----------------------------------------------------------------------------
//
// SWBROKER_VERSION
//
#define SWBROKER_VERSION "post-0.2.0"



// -----------------------------------------------------------------------------
//
// Command line arguments
//
unsigned short port         = 1026;
char*          dbName       = "mongoc";
char*          apiNames     = NULL;
unsigned int   prettySpaces = 0;
bool           localOnly    = false;
bool           fg           = false;
int            poolSize     = 32;
char*          corsOrigin   = NULL;
int            corsMaxAge   = 86400;
char*          userContext  = NULL;
char*          csourceAlias = NULL;
bool           noSplitEntities = false;

static KArg kargV[] =
{
  { "--port",               "-p",           KaUShort, _vp &port,         KaOpt, _vp 1026,     _vp 1, _vp 65535, "TCP port to listen on" },
  { "--database",           "-db",          KaString, _vp &dbName,       KaOpt, _vp "mongoc", NULL,  NULL,      "database plugin (short name or full path)" },
  { "--apiPlugins",         "-api",         KaString, _vp &apiNames,     KaOpt, _vp NULL,      NULL,  NULL,      "API plugins (comma-separated)" },
  { "--pretty-print",       "-pp",          KaUInt,   _vp &prettySpaces, KaOpt, _vp 0,         _vp 0, _vp 16,   "default JSON indentation (0=compact)" },
  { "--connectionPoolSize", "-cps",         KaInt,    _vp &poolSize,     KaOpt, _vp 32,        _vp 1, _vp 200,  "MHD thread pool size" },
  { "--localOnly",          "-local",       KaBool,   _vp &localOnly,    KaOpt, _vp KFALSE,    _vp KFALSE, _vp KTRUE, "local-only mode (no distributed operations)" },
  { "--corsOrigin",         "-corsOrigin",  KaString, _vp &corsOrigin,   KaOpt, _vp NULL,      NULL,  NULL,      "enable CORS with allowed origin ('__ALL' for any)" },
  { "--corsMaxAge",         "-corsMaxAge",  KaInt,    _vp &corsMaxAge,   KaOpt, _vp 86400,     _vp 0, _vp 864000, "preflight cache max age in seconds" },
  { "--userContext",        "-ctx",         KaString, _vp &userContext,  KaOpt, _vp NULL,      NULL,  NULL,      "default user @context URL" },
  { "--csourceAlias",       "-csourceAlias",KaString, _vp &csourceAlias, KaOpt, _vp NULL,      NULL,  NULL,      "contextSourceAlias base for Via headers (default: <exe>:<port>)" },
  { "--noSplitEntities",    "-noSplitEntities",KaBool, _vp &noSplitEntities,KaOpt, _vp false, _vp false, _vp true, "disable split entities — each entity fully at one source" },
  { "--foreground",         "-fg",          KaBool,   _vp &fg,           KaOpt, _vp KFALSE,    _vp KFALSE, _vp KTRUE, "run in foreground (don't daemonize)" },
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
// brokerPreServiceHook - chain the tenant + metrics pre-service work
//
// Tenant hook goes first; on its failure we skip the metric bump — a
// request that failed tenant resolution is effectively rejected before
// it hits a service routine.
//
static bool brokerPreServiceHook(void)
{
  if (!tenantPreServiceHook())
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
  ldSplitEntities       = !noSplitEntities;  // default: true (split entities is standard NGSI-LD behavior)
  ldDefaultContextUrl   = userContext;
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

  int r = ktInit("swBroker", NULL, true, NULL, NULL, kaBuiltinVerbose, kaBuiltinDebug, false);
  if (r != 0)
    KT_X(1, "ktInit failed");

  KT_V("swBroker  %s", SWBROKER_VERSION);

  signal(SIGINT,  onSignal);
  signal(SIGTERM, onSignal);
  signal(SIGSEGV, onCrash);

  if (swRestClientInit(4, 60, "swBroker") != 0)
    KT_X(1, "swRestClientInit failed");

  static KAlloc  contextAlloc;
  static char    contextBuffer[64 * 1024];

  kaBufferInit(&contextAlloc, contextBuffer, sizeof(contextBuffer), 0, NULL, "jsonld-context");

  if (swldInit(&contextAlloc, NULL, contextDownload) != 0)
    KT_X(1, "swldInit failed");

  if (ldInit() != 0)
    KT_X(1, "ldInit failed");

  forwardingHttpRegister();

  if (prettySpaces > 0)
    swRestSetPrettySpaces(prettySpaces);

  if (corsOrigin != NULL)
  {
    const char* origin = (strcmp(corsOrigin, "__ALL") == 0) ? "*" : corsOrigin;

    SwRestCorsConfig corsConf = {
      .allowOrigin   = origin,
      .allowHeaders  = "Content-Type, Accept, Link, NGSILD-Tenant, NGSILD-Path",
      .exposeHeaders = "Location, NGSILD-Results-Count, Link, NGSILD-Tenant",
      .maxAge        = corsMaxAge
    };
    swRestCorsConfig(&corsConf);
  }

  apiPluginsInit();
  tenantInit("sw");
  metricsInit();
  ldNotifyStatsHookSet(brokerNotifyStatsHook);
  swRestSetPreServiceHook(brokerPreServiceHook);
  swRestSetPostResponseHook(brokerPostResponseHook);

  if (dbStart() != 0)
    KT_X(1, "dbStart failed");

  //
  // Load subscriptions from DB into cache.
  // mongoc uses swRest.kalloc internally — set up a startup buffer for it.
  // Cache items get cloned into persistent (malloc) storage, so this is short-lived.
  //
  static char startupKallocBuf[16384];
  kaBufferInit(&swRest.kalloc, startupKallocBuf, sizeof(startupKallocBuf), 4096, NULL, "startup");
  tenantSubCacheReload();
  tenantRegCacheReload();
  contextCacheReload();
  kaBufferReset(&swRest.kalloc, false);

  // Start pernot loop (periodic notification background thread)
  if (tenant0.pernotCacheP != NULL)
    ldPernotLoopStart((LdPernotCache*) tenant0.pernotCacheP, pernotQueryCallback);

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

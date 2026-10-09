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
#include <semaphore.h>                            // sem_t, sem_init, sem_post, sem_wait
#include <string.h>                               // strcmp, memcpy, strpbrk
#include <time.h>                                 // time
#include <stdint.h>                               // uint32_t
#include <ifaddrs.h>                              // getifaddrs, freeifaddrs, struct ifaddrs
#include <net/if.h>                               // IFF_LOOPBACK, IFF_UP, IFF_RUNNING, IFF_POINTOPOINT
#include <sys/socket.h>                           // AF_INET
#include <netinet/in.h>                           // struct sockaddr_in
#include <arpa/inet.h>                            // inet_ntop, ntohl, INET_ADDRSTRLEN

#include "corAlloc/corAlloc.h"                    // CorAlloc, corAllocBufferInit, corAllocThreadSafe
#include "corLog/corLog.h"                        // COR_I, COR_V, COR_X
#include "corLog/corLogGlobals.h"                  // corLogInfo, corLogVerbose, corLogDebug
#include "corBase/corCpuCount.h"                   // corCpuCount
#include "corBase/corBaseInit.h"                   // corBaseInit, corBaseTraceLevelsSet
#include "corArgs/corArgs.h"                      // corArgsInit, corArgsParse, corArgsPeek, CorArg, CorArgsStatus, corArgsStatus, CORARGS_END, corArgsUsage
#include "corPlugin/corPlugin.h"                    // corPluginSetBaseDir, corPluginBaseDir, corPluginArgUpdate
#include "corRest/corRest.h"                        // corRestInit, corRestSetPrettySpaces, corRestSetPreServiceHook, corRestParamAdd
#include "corRest/corRestBackend.h"                  // corRestHttpLoopsSet
#include "corRest/corRestStop.h"                     // corRestStop
#include "corRest/corRestClient.h"                  // corRestClientInit, corRestClientTlsInsecureSet
#include "corJsonld/corJsonld.h"                    // corLdConcurrencySet, CORJSONLD_VERSION
#include "corJsonld/CorLdContext.h"                 // CorLdContext, CorLdContextKind
#include "corJsonld/CorLdContextCache.h"            // CorLdContextCache
#include "corJsonld/corLdCache.h"                   // corLdCacheInsert
#include "corJsonld/corLdContextParse.h"            // corLdContextFromObject

#include "corJson/CorJson.h"                      // CorJson
#include "corJson/corJsonCreate.h"                // corJsonCreate
#include "corJson/corJsonParse.h"                 // corJsonParse
#include "corTree/corTreeLookup.h"                // corTreeLookup
#include "corTree/corTreeBuilder.h"               // corTreeChildAdd
#include "corTree/corTreeClone.h"                 // corTreeClone
#include "corAlloc/corAlloc.h"                    // corAlloc
#include "corAlloc/corAllocStrdup.h"              // corAllocStrdup
#include "corNgsild/LdSnapshotCache.h"                // ldSnapshotCacheDestroyHookSet, ldSnapshotRequestRelease
#include "corNgsild/ldEntityMap.h"                    // ldEntityMapRequestRelease, ldEntityMapMaxBytes
#include "corNgsild/corNgsild.h"                    // ldInit, CORNGSILD_VERSION, ldParamsInit
#include "corNgsild/ldUrlWildcardCheck.h"          // ldUrlWildcardCheck
#include "corNgsild/ldHooks.h"                      // ldAcceptPrecondition
#include "corNgsild/ldNotifyDefer.h"               // ldNotifyDispatchPending
#include "corNgsild/ldRegCache.h"                  // ldRegCacheProbePending
#include "corNgsild/ldNotifyStatsHook.h"           // ldNotifyStatsHookSet
#include "corNgsild/ldLinkedEntitiesHook.h"        // ldLinkedEntitiesHookSet
#include "linkedEntities/ldLinkedEntities.h"      // ldLinkedEntitiesNotifApiArray
#include "corNgsild/LdPernotCache.h"               // LdPernotCache, LdPernotItem
#include "corNgsild/LdTypeExpr.h"                   // ldTypeExprParse
#include "corNgsild/LdVocab.h"                      // LD_VOCAB_ENTITIES
#include "corNgsild/ldPernotLoop.h"                // ldPernotLoopStart
#include "corNgsild/ldPeriodicLoop.h"              // ldPeriodicLoopStart, ldPeriodicLoopStop
#include "corNgsild/ldContextHost.h"               // ldContextHostReaperStart
#include "corNgsild/ldCsrSubNotify.h"              // ldCsrSubPeriodicLoopRegister, ldCsrSubDispatchPending
#include "corNgsild/ldTenantCaches.h"              // LdTenantCaches, LdTenantCachesFn
#include "corNgsild/ldCheckSubscription.h"         // ldSubEntityTypeExprsRelease
#include "corNgsild/ldStatsFlushLoop.h"            // ldStatsFlushLoopStart
#include "metrics/subStatsFlushAll.h"             // subStatsFlushAll
#include "corNgsild/CorNgsild.h"                  // corNgsild, ldCsourceAliasBase
#include "corNgsild/ldError.h"                     // ldError
#include "corNgsild/LdProblem.h"                    // LD_ERROR_LD_CONTEXT_NOT_AVAILABLE

#include "db/snapshotTenant.h"                     // snapshotItemDestroy, snapshotTenantsVisit
#include "db/DbDriver.h"                          // db, DB_OK
#include "db/DbQueryFilter.h"                     // DbQueryFilter
#include "db/dbInit.h"                            // dbStart
#include "db/dbClose.h"                           // dbClose
#include "db/Tenant.h"                            // tenantPreServiceHook, tenantApiName
#include "db/contextCache.h"                      // contextCacheReload
#include "ha/haInit.h"                            // haInit, haChannel
#include "serviceRoutines/ldSnapshotRead.h"       // ldSnapshotWriteGuard

#include "troe/TroeDriver.h"                      // troe
#include "troe/troeInit.h"                        // troeStart, troeStop
#include "troe/troeDispatch.h"                    // troeDispatchPending
#include "db/dbExpiredEntities.h"                 // dbExpiredEntityDispatchPending

#include "plugin/ApiPlugin.h"                     // ApiPlugin, apiPlugins, apiPluginCount
#include "startup/startupCoreContext.h"            // startupCoreContext
#include "startup/startupLog.h"                    // startupLog
#include "startup/startupArena.h"                  // startupArena
#include "plugin/pluginLoader.h"                  // pluginStoresLoad, pluginLoadApi, pluginLoadBridges, pluginArgsAdd
#include "bridge/bridgeNotify.h"                  // bridgeNotifyInit
#include "corBridge/BridgeDriver.h"                // BridgeDriver, bridges, bridgeCount, BRIDGES_MAX
#if COR_FEATURE_TRANSPORTS
#include "transport/transport.h"                   // transportLoad, transportInit, transportStop
#endif

#include "bridge/bridgeService.h"                   // bridgeServiceUpdateIn, bridgeServiceApplySet
#if COR_FEATURE_SERVICE_EXECUTION
#include "serviceExecution/seExecution.h"            // seExecutionTick, seExecutionRetentionNs
#include "serviceExecution/seRequest.h"              // SE_PARAM_*
#endif
#include "bridge/channelCache.h"                  // channelCacheInit, channelCacheFirst, Channel
#include "bridge/channelConfigLoad.h"             // channelConfigLoad
#include "bridge/channelPrePopulate.h"            // channelPrePopulate
#include "bridge/bridgeSampleIn.h"                // bridgeSampleIn, bridgeSampleQualifiedIn
#include "bridge/bridgeGoal.h"                        // bridgeGoalEventIn
#include "bridge/bridgeServiceSync.h"             // bridgeReplyIn, bridgeSyncDefault, bridgeSyncTimeoutMs, bridgeRequestsReleasePending
#include "coraineTraceLevels.h"                    // CtBridge

#if COR_FEATURE_REGISTRATIONS
#include "forwarding/forwardingHttp.h"            // forwardingHttpRegister
#include "forwarding/forwardingCor.h"             // forwardingCorRegister
#endif

#include "corBase/corCo.h"                      // corCoCurrent
#include "corRest/corRestWait.h"                // corRestWaitFd
#include "corRest/corRestCor.h"                   // corRestCorInit, corRestCorListen - cor:// serves with or without registrations
#include "corNgsild/ldBinCodec.h"                 // ldBinCodec, ldBinNamespaceV
#include "corNgsild/CorTerm.h"                    // CorTermLast

#include "metrics/metrics.h"                      // metricsInit, metricsPreService, metricsPostResponse, metricsNotificationSent, metricsCsrNotificationSent

#include "coraineFeatures.h"                      // coraineFeatures
#include "ngsildServices.h"                       // ngsildCoreServices, serviceBuild
#include "crashReport.h"                          // crashReportInstall
#include "inlineDispatch.h"                                  // inlineDispatchInit
#include "memoryBudget.h"                                    // memoryBudgetInit, memoryBudgetAdmit
#if COR_FEATURE_HEALTH
#include "health.h"                                          // healthStart, healthRequestEnd, ...
#endif
#include "serviceRoutines/getEntities.h"                     // autoEntityMaps, AutoEntityMaps, GET_ENTITIES_PARAM_PAGE
#if COR_FEATURE_RESPONSE_BUDGET
#include "serviceRoutines/responseBudget.h"                  // responseBudgetBytes
#endif



// -----------------------------------------------------------------------------
//
// contextOwner / contextSleep - corLdConcurrencySet: who asks for an @context, and how to wait for it
//
// On a coroutine the asker is the request, not the thread - other requests run on the same thread
// (doc/coroutines.md) - and a wait for another's download must yield, not stop the thread the download
// runs on: corRestWaitFd with no fd is a timer, a yield inside a coroutine and a poll() elsewhere.
//
static uintptr_t contextOwner(void)
{
  CorCo* coP = corCoCurrent();

  return (coP != NULL) ? (uintptr_t) coP : (uintptr_t) pthread_self();
}

static void contextSleep(int ms)
{
  corRestWaitFd(-1, 0, ms, NULL);
}



#include "coraineVersion.h"                      // CORAINE_VERSION



// -----------------------------------------------------------------------------
//
// Command line arguments
//
unsigned short port         = 1026;
unsigned short corPort      = 0;           // cor:// - the binary API (0: off)
char*          dbName       = "mongoc";
char*          troeName     = "none";
char*          apiNames     = NULL;
char*          bridgeNames  = NULL;
#if COR_FEATURE_TRANSPORTS
char*          transportNames = NULL;
#endif
char*          bridgeConfig = NULL;
unsigned int   prettySpaces = 0;
bool           notifyValueChangeOnly = false;
bool           fg           = false;
bool           versionOnly  = false;   // --version: handled before corArgsInit; in the table so --usage lists it
int            poolSize     = 0;
int            httpLoops    = 0;   // 0: auto - see the corCpuCount call in main()
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
bool           noInline        = false;     // hand every request to a worker (see inlineDispatch.h)
int            memoryLimit     = 0;         // MiB; 0: 85% of the cgroup limit, if there is one (see memoryBudget.h)
#if COR_FEATURE_HEALTH
unsigned short healthPort      = 0;         // 0: no health port (see health.h)
int            healthStallTimeout = 30;     // seconds a request may be in flight with none finishing before /live answers 503
#endif
int            maxRequestSize  = 2;          // MiB; § 6.3.2 413 threshold (0 = no cap)
#if COR_FEATURE_RESPONSE_BUDGET
int            maxResponseSize = -1;         // MiB; the byte budget of an entity query (0 = none; -1 = 1/16 of the memory budget, none without one - see responseBudget.h)
#endif
#if COR_FEATURE_AUTO_ENTITY_MAP
char*          autoEntityMapsArg = (char*) "distributed";   // --autoEntityMaps: none | distributed | all (getEntities.h)
#endif
int            entityMapMemory = 64;         // MiB; the memory all EntityMaps may hold together (0 = no cap, and no automatic maps - see ldEntityMap.h)
int            subStatsFlushInterval = 60;   // seconds; 0 disables the timer
int            cooldownMillis        = 30000; // --cooldownMillis; default endpoint cooldown after failure (0 = off)

static CorArg kargV[] =
{
  { "--traceLevels",        "-t",           CorArgString, _vp &traceLevels,  CorArgOpt, _vp NULL,  NULL,  NULL,      "trace levels" },
  { "--port",               "-p",           CorArgUShort, _vp &port,     CorArgOpt, _vp 1026, _vp 1, _vp 65535, "TCP port to listen on" },
  { "--corPort",            "-corPort",     CorArgUShort, _vp &corPort,  CorArgOpt, _vp 0,    _vp 0, _vp 65535, "TCP port for cor:// - the binary API (0: off)" },
  { "--database",           "-db",          CorArgString, _vp &dbName,   CorArgOpt, _vp "mongoc", NULL,  NULL,      "database plugin (short name or full path)" },
  { "--troe",               "-troe",        CorArgString, _vp &troeName, CorArgOpt, _vp "none",   NULL,  NULL,      "TRoE temporal-storage plugin (short name or full path; 'none' disables)" },
  { "--troeSync",           "-troeSync",    CorArgBool,   _vp &troeSync,    CorArgOpt, _vp false, _vp false, _vp true, "record TRoE writes BEFORE the response, so a temporal read sees them at once; default defers them until after it" },
  { "--apiPlugins",         "-api",         CorArgString, _vp &apiNames, CorArgOpt, _vp NULL,  NULL,  NULL,      "API plugins (comma-separated)" },
  { "--bridges",            "-br",          CorArgString, _vp &bridgeNames,  CorArgOpt, _vp NULL,  NULL,  NULL,      "bridge plugins - transports to non-NGSI-LD peers (comma-separated)" },
#if COR_FEATURE_TRANSPORTS
  { "--transports",         "-transports",  CorArgString, _vp &transportNames, CorArgOpt, _vp NULL, NULL, NULL,      "transport plugins - the API over another protocol: ws (WebSocket, on GET /ngsi-ld/v1/ws)" },
#endif
  { "--bridgeConfig",       "-brc",         CorArgString, _vp &bridgeConfig, CorArgOpt, _vp NULL,  NULL,  NULL,      "bridge configuration file (Channels, and each bridge's own settings)" },
  { "--ddsSync",            "-ddsSync",     CorArgBool,   _vp &bridgeSyncDefault,   CorArgOpt, _vp false, _vp false, _vp true, "a PATCH that writes a service attribute waits for the service's reply by default (?ddsSync=false opts out); default: it does not wait" },
  { "--ddsSyncTimeout",     "-ddsSyncTimeout", CorArgInt, _vp &bridgeSyncTimeoutMs, CorArgOpt, _vp 0, _vp 0, _vp 600000, "how long, in milliseconds, a waiting PATCH (ddsSync) gives a service to answer before answering 202 - the reply then lands when it comes (0: the bridge configuration's syncTimeoutMs, else 200)" },
  { "--ddsSyncWaitMax",     "-ddsSyncWaitMax", CorArgInt, _vp &bridgeSyncWaitMax,   CorArgOpt, _vp 8,    _vp 0, _vp 200,    "at most this many requests wait for a service at once - the rest send without waiting (202), so a slow DDS network cannot take every worker" },
  { "--pretty-print",       "-pp",          CorArgUInt,   _vp &prettySpaces, CorArgOpt, _vp 0,     _vp 0, _vp 16,   "default JSON indentation (0=compact)" },
  { "--connectionPoolSize", "-cps",         CorArgInt,    _vp &poolSize,     CorArgOpt, _vp 0,     _vp 0, _vp 200,  "MHD thread pool size (0: the cores, for a store that never waits; 32 otherwise)" },
  { "--httpLoops",          "-hl",          CorArgInt,    _vp &httpLoops,    CorArgOpt, _vp 0,     _vp 0, _vp 64,   "HTTP event loops sharing the port, 0: one per core, max 4 (built-in server only)" },
  { "--notifyValueChangeOnly", "-nvco",     CorArgBool,   _vp &notifyValueChangeOnly, CorArgOpt, _vp false, _vp false, _vp true, "only notify when an attribute value changed (suppress value-neutral updates)" },
  { "--corsOrigin",         "-corsOrigin",  CorArgString, _vp &corsOrigin,   CorArgOpt, _vp NULL,  NULL,  NULL,      "enable CORS with allowed origin ('__ALL' for any)" },
  { "--corsMaxAge",         "-corsMaxAge",  CorArgInt,    _vp &corsMaxAge,   CorArgOpt, _vp 86400, _vp 0, _vp 864000, "preflight cache max age in seconds" },
  { "--defaultUserContext", "-duc",         CorArgString, _vp &defaultUserContext, CorArgOpt, _vp NULL, NULL,  NULL,      "default user @context URL" },
  { "--csourceAlias",       "-csourceAlias",CorArgString, _vp &csourceAlias, CorArgOpt, _vp NULL,  NULL,  NULL,      "contextSourceAlias base for Via headers (default: the advertised endpoint authority)" },
  { "--httpEndpoint",       "-he",          CorArgString, _vp &httpEndpoint, CorArgOpt, _vp NULL,  NULL,  NULL,      "externally-reachable HTTP base URL (default: auto-detected LAN IP, else http://localhost:<port>)" },
  { "--contextSourceExtras","-csx",         CorArgString, _vp &contextSourceExtras, CorArgOpt, _vp NULL, NULL, NULL,  "path to a JSON file rendered verbatim on /info/sourceIdentity (§ 5.2.40)" },
  { "--distributed",        "-dist",        CorArgBool,   _vp &distributed, CorArgOpt, _vp false, _vp false, _vp true, "enable distributed operations (forward to registered Context Sources); off by default — the Registry API works either way" },
  { "--noSplitEntities",    "-noSplitEntities",CorArgBool, _vp &noSplitEntities,CorArgOpt, _vp false, _vp false, _vp true, "disable split entities — each entity fully at one source" },
  { "--high-precision",     "-hp",          CorArgBool,   _vp &highPrecision, CorArgOpt, _vp false, _vp false, _vp true, "render DateTime values (createdAt/modifiedAt/observedAt/expiresAt) at full nanosecond precision (9 digits); default is 6 (§5.2.2.4 µs)" },
  { "--asyncSnapshot",      "-asyncSnapshot",  CorArgBool, _vp &asyncSnapshot, CorArgOpt, _vp false, _vp false, _vp true, "run snapshotQueries in a background thread (POST returns 201 immediately, status=preparing)" },
  { "--maxRequestSize",     "-mrs",            CorArgInt,  _vp &maxRequestSize, CorArgOpt, _vp 2,    _vp 0,    _vp 4096,  "max request body size in MiB (0 = no cap; § 6.3.2 413 threshold)" },
#if COR_FEATURE_RESPONSE_BUDGET
  { "--maxResponseSize",    "-maxResponseSize", CorArgInt, _vp &maxResponseSize, CorArgOpt, _vp -1, _vp -1,   _vp 4096,  "byte budget of an entity query in MiB - a page ends before the entity that would pass it, a query that needs more at once (orderBy) gets 403 TooManyResults (0 = no budget; -1 = 1/16 of the memory budget, none without one)" },
#endif
#if COR_FEATURE_AUTO_ENTITY_MAP
  { "--autoEntityMaps",     "-autoEntityMaps", CorArgString, _vp &autoEntityMapsArg, CorArgOpt, _vp "distributed", NULL, NULL, "which queries of more than one page get an EntityMap the client did not ask for: none, distributed (the queries forwarded to Context Sources - the default) or all (local ones too)" },
#endif
  { "--entityMapMemory",    "-entityMapMemory", CorArgInt, _vp &entityMapMemory, CorArgOpt, _vp 64, _vp 0,    _vp 65536, "memory all EntityMaps may hold together, in MiB - an automatic map that does not fit is not made (the least recently used ones give way first), a requested one gets 403 TooManyResults (0 = no cap, and no automatic EntityMaps)" },
  { "--subStatsFlushInterval","-ssfi",      CorArgInt,    _vp &subStatsFlushInterval, CorArgOpt, _vp 60, _vp 0, _vp 86400, "sub-stats periodic flush interval (s; 0 = off)" },
  { "--distOpTimeout",      "-dtmo",        CorArgInt,    _vp &corRestClientDefaultRequestTimeoutMs, CorArgOpt, _vp 5000, _vp 1, _vp 600000, "default HTTP client request timeout (ms) — distop forwards, sub-notifs, @context downloads" },
  { "--cooldownMillis",     "-cms",         CorArgInt,    _vp &cooldownMillis, CorArgOpt, _vp 30000, _vp 0, _vp 86400000, "default endpoint cooldown after a notification/forward failure (ms; 0 = only when the subscription/registration specifies one)" },
  { "--version",            "-V",           CorArgBool,   _vp &versionOnly,  CorArgOpt, _vp false,    _vp false, _vp true, "print version and exit" },
  { "--foreground",         "-fg",          CorArgBool,   _vp &fg,           CorArgOpt, _vp false,    _vp false, _vp true, "run in foreground (don't daemonize)" },
  { "--insecureNotif",      "-insecureNotif",CorArgBool,  _vp &insecureNotif, CorArgOpt, _vp false, _vp false, _vp true, "accept self-signed certificates on TLS notifications/forwards (endpoint inside a trusted network)" },
  { "--noInline",           "-noInline",    CorArgBool,  _vp &noInline,    CorArgOpt, _vp false, _vp false, _vp true, "hand every request to a worker thread - no request runs on the I/O thread that read it" },
  { "--memoryLimit",        "-memoryLimit", CorArgInt,   _vp &memoryLimit, CorArgOpt, _vp 0,     _vp 0,     _vp 1048576, "memory budget in MiB - over 90% of it writes are refused (503), over all of it everything but deletes and monitoring (0: 85% of the container's memory limit, none outside a container)" },
#if COR_FEATURE_HEALTH
  { "--healthPort",         "-healthPort",  CorArgUShort, _vp &healthPort,   CorArgOpt, _vp 0,     _vp 0,     _vp 65535, "TCP port for the health probes - GET /live, GET /ready and a JSON report, served outside the HTTP server (0: off)" },
  { "--healthStallTimeout", "-healthStallTimeout", CorArgInt, _vp &healthStallTimeout, CorArgOpt, _vp 30, _vp 1, _vp 3600, "seconds a request may be in flight, with no request finishing, before GET /live on the health port answers 503" },
#endif
  { "--high-availability",  "-ha",          CorArgString, _vp &haChannel,    CorArgOpt, _vp NULL,  NULL,  NULL,      "keep the caches in sync with the other broker instances ('mongo' = change streams, needs a replica set; <ip:port> = the haaux server)" },
  CORARGS_END
};



// shutdownSem - posted by the signal handler, waited on by main
//
static sem_t shutdownSem;



// -----------------------------------------------------------------------------
//
// onSignal - SIGINT / SIGTERM: wake main, which shuts down in order
//
// Nothing but sem_post here. The handler runs in whichever thread the kernel
// picked - a request worker, quite possibly - and the shutdown joins those
// workers; a worker cannot join itself. sem_post is async-signal-safe.
//
static void onSignal(int sigNo)
{
  (void) sigNo;
  sem_post(&shutdownSem);
}



// -----------------------------------------------------------------------------
//
// shutdownInOrder -
//
// bridgesClose is defined further down, beside bridgesInit and the rest of the
// bridge seam; this is the one caller that comes before it.
//
static void bridgesClose(void);

static void shutdownInOrder(void)
{
#if COR_FEATURE_HEALTH
  healthStopping();   // /ready answers 503 from here on: an orchestrator stops sending traffic
#endif

  //
  // The HTTP side FIRST: stop taking requests, let the queued ones finish, join
  // the request workers. Everything after this tears down what a request uses -
  // stopped with requests still in flight, a worker inside a PATCH was reading a
  // mongoc pool that dbClose had destroyed, and the broker died of SIGSEGV on its
  // way out (under load, one stop in two).
  //
  corRestStop();

  //
  // Stop the periodic loop before the DB. Its dispatch thread calls into the DB
  // plugin (pernot re-queries the entities a periodic subscription watches), so
  // tearing the plugin down underneath it leaves whatever that thread had checked
  // out unreturned - a mongoc client and its guts, ~8.8 KB, which is what the
  // nightly valgrind run reported against subscription_pernot. Worse than the
  // leak: a thread still inside entityQuery would be reading a pool that
  // dbClose has already destroyed.
  //
  // ldPeriodicLoopStop clears the run flag and joins, so on return no other
  // thread can be inside the plugin.
  //
  //
  // Bridges before the periodic loop, and both before dbClose: a bridge thread
  // is an inbound writer the broker does not own, and close() is what joins it.
  //
#if COR_FEATURE_TRANSPORTS
  transportStop();                                  // its connections closed: their subscriptions deleted
#endif
  bridgesClose();

  ldPeriodicLoopStop();

#if COR_FEATURE_HEALTH
  healthStop();       // the pingers stopped: they ping with what dbClose frees
#endif

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
// Called between corArgsInit and corArgsParse so that plugin-contributed args
// are known before parsing.
//
static bool pluginsLoad(int argC, char* argV[])
{
  bool startupError = false;

  //
  // Which plugins to load has to be known BEFORE corArgsParse, because the plugins
  // contribute options of their own to the table that parse then resolves. So
  // each of the four is taken from the command line by corArgsPeek, falling back
  // to the variable behind the option.
  //
  // That fallback is not "the default" - corArgsInit has already run (see main)
  // and has resolved CORAINE_DATABASE / CORAINE_TROE / CORAINE_APIPLUGINS /
  // CORAINE_BRIDGES into
  // these variables, so it is "the environment, or failing that the default".
  // corArgsPeek itself reads argv and nothing else - it has no idea an
  // environment exists.
  //
  // --apiPlugins had no fallback line, which is why it alone ignored its
  // environment variable. dbName and troeName carry compiled-in defaults that
  // made the omission invisible there ("mongoc"/"none" either way), while
  // apiNames starts NULL, so the `if (apiPeek != NULL)` guard below skipped
  // loading any API plugin at all. Found by a tutorial author configuring the
  // container by environment - which is what container users do, and what no
  // functest here does.
  //
  char* dbPeek = corArgsPeek(argC, argV, kargV, "--database");
  if (dbPeek == NULL)
    dbPeek = dbName;  // use default

  char* troePeek = corArgsPeek(argC, argV, kargV, "--troe");
  if (troePeek == NULL)
    troePeek = troeName;  // use default ("none")

  char* apiPeek = corArgsPeek(argC, argV, kargV, "--apiPlugins");
  if (apiPeek == NULL)
    apiPeek = apiNames;  // CORAINE_APIPLUGINS - corArgsInit ran above and has already resolved it

  char* bridgePeek = corArgsPeek(argC, argV, kargV, "--bridges");
  if (bridgePeek == NULL)
    bridgePeek = bridgeNames;  // CORAINE_BRIDGES - and it needs this line for the same reason --apiPlugins did

  //
  // The DB and the TRoE plugin (dlopen + register, no connection yet) - as coraine-import loads them
  //
  if (pluginStoresLoad(dbPeek, troePeek) == true)
    startupError = true;

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
          pluginArgsAdd(apiPlugins[i].args);
      }
    }
  }

  //
  // Load bridge plugins (dlopen + bridgeRegister for each)
  //
  // Only the .so is loaded here. A bridge's init() - which is what actually
  // brings a transport up, and after which samples start arriving on threads
  // the broker does not own - runs much later, once there is somewhere for one
  // to land.
  //
  if (bridgePeek != NULL)
  {
    char errBuf[1024];
    if (pluginLoadBridges(bridgePeek, errBuf, sizeof(errBuf)) != 0)
    {
      fprintf(stderr, "%s\n", errBuf);
      startupError = true;
    }
    else
    {
      for (int i = 0; i < bridgeCount; i++)
      {
        if (bridges[i].args != NULL)
        {
          static char bridgeSepText[BRIDGES_MAX][128];
          if (bridges[i].alias != NULL)
            snprintf(bridgeSepText[i], sizeof(bridgeSepText[i]), "Bridge (%s) plugin options:", bridges[i].alias);
          else
            snprintf(bridgeSepText[i], sizeof(bridgeSepText[i]), "Bridge plugin options:");

          static CorArg bridgeSepArgV[BRIDGES_MAX][2];
          bridgeSepArgV[i][0] = (CorArg) CORARGS_SEPARATOR(NULL);
          bridgeSepArgV[i][1] = (CorArg) CORARGS_END;
          bridgeSepArgV[i][0].description = bridgeSepText[i];
          pluginArgsAdd(bridgeSepArgV[i]);
          pluginArgsAdd(bridges[i].args);
        }
      }
    }
  }

  //
  // Add available-plugin info (shown in -u usage output)
  //
  corPluginArgUpdate("--apiPlugins", "api");
  corPluginArgUpdate("--bridges", "bridge");

  // Add footer showing plugin directory
  static char footerText[256];
  snprintf(footerText, sizeof(footerText), "Plugins are loaded from %s", corPluginBaseDir());
  static CorArg footerArgV[] = { CORARGS_SEPARATOR(NULL), CORARGS_END };
  footerArgV[0].description = footerText;
  pluginArgsAdd(footerArgV);

  return startupError;
}



// -----------------------------------------------------------------------------
//
// bridgeParams - the URL parameters the broker itself adds to NGSI-LD's
//
// ?ddsSync - accepted on every route (see main), acted on by the three entity
// PATCH routes - see bridgeServiceSync.h.
//
static CorRestParam bridgeParams[] =
{
  { "ddsSync", BRIDGE_PARAM_DDS_SYNC },
  { NULL,      0                     }
};



// -----------------------------------------------------------------------------
//
// pageParams - the page position of GET /entities (getEntities.h): the links of a local query name
// the entity a page starts after (or ends before) instead of an offset
//
static CorRestParam pageParams[] =
{
  { "pageAfter",  GET_ENTITIES_PARAM_PAGE },
  { "pageBefore", GET_ENTITIES_PARAM_PAGE },
  { NULL,         0                       }
};



#if COR_FEATURE_SERVICE_EXECUTION
// -----------------------------------------------------------------------------
//
// serviceParams - the URL parameters of GET /ngsi-ld/v1/services (Service Execution)
//
static CorRestParam serviceParams[] =
{
  { "entityId",        SE_PARAM_SERVICES_QUERY   },
  { "serviceName",     SE_PARAM_SERVICES_QUERY   },
  { "executionStatus", SE_PARAM_SERVICES_QUERY   },
  { "includeServices", SE_PARAM_INCLUDE_SERVICES },
  { "serviceDetails",  SE_PARAM_SERVICE_DETAILS  },
  { NULL,              0                         }
};
#endif



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
        COR_X(1, "corRestParamAdd failed for API plugin '%s'", p->alias ? p->alias : "?");
    }

    if (p->init != NULL)
    {
      if (p->init() != 0)
        COR_X(1, "init failed for API plugin '%s'", p->alias ? p->alias : "?");
    }
  }
}



// -----------------------------------------------------------------------------
//
// bridgeLogFunction - the broker's side of BridgeBroker::logFunction
//
// ⚠ The file/line/function are the PLUGIN's, so they are passed to corLogOut rather
// than captured here. The KT_* macros record their own position, which inside a
// log helper is this file, every time.
//
static void bridgeLogFunction(int severity, const char* fileName, int lineNo, const char* funcName, const char* msg)
{
  char sev;

  switch (severity)
  {
  case BRIDGE_LOG_ERROR:    sev = 'E';  break;
  case BRIDGE_LOG_WARNING:  sev = 'W';  break;
  case BRIDGE_LOG_INFO:     sev = 'I';  break;
  case BRIDGE_LOG_DEBUG:    sev = 'D';  break;
  default:                  sev = 'T';  break;
  }

  corLogOut((char*) fileName, lineNo, (char*) funcName, sev, -1, "%s", msg);
}



// -----------------------------------------------------------------------------
//
// bridgeBroker - what every loaded bridge plugin is handed
//
static BridgeBroker bridgeBroker =
{
  BRIDGE_ABI_VERSION,
  bridgeSampleIn,
  bridgeLogFunction,
  bridgeSampleQualifiedIn,
  bridgeReplyIn,
  bridgeGoalEventIn,
  bridgeGoalEventPartIn,
  bridgeSampleMetaIn,
  bridgeReplyMetaIn,
  bridgeGoalEventMetaIn,
  bridgeReplyExchangeIn,
  bridgeEndpointDiscoveredIn,
  bridgeServiceUpdateIn
};



// -----------------------------------------------------------------------------
//
// bridgesInit - bring every loaded bridge's transport up
//
// Called LATE in startup, deliberately: the moment a transport is up, samples
// arrive on threads of its own, and they must have somewhere to land. The DB is
// open, the caches are loaded and the hooks are set by the time this runs.
//
// -----------------------------------------------------------------------------
//
// bridgeChannelsInit - build the Channels, and the entities they write into
//
// ⚠ RUNS WHILE THE STARTUP ALLOCATOR IS STILL ALIVE, and that is why it is not
// part of bridgesInit below. The DB plugins allocate what they read through
// corRest.kalloc, and the startup buffer is torn down - corAllocBufferReset with
// reuse false, which frees every block and leaves allocList pointing at them -
// once the caches are loaded. Touching the database after that point means
// allocating from an arena that has been freed.
//
// Splitting it this way is also the right shape on its own terms: every
// endpoint and every entity exists before a single transport is opened, so
// there is no window in which arriving samples are dropped as unclaimed.
//
static void bridgeChannelsInit(void)
{
  if (bridgeCount == 0)
    return;

  if (channelCacheInit() != CHANNEL_OK)
    COR_X(1, "unable to create the channel cache");

  int channels = channelConfigLoad(bridgeConfig, (bridgeConfig != NULL), &tenant0);

  if (channels > 0)
  {
    int attrs = channelPrePopulate(&tenant0);

    if (attrs > 0)
      COR_I("%d channel attribute%s created as placeholders", attrs, (attrs == 1) ? "" : "s");
  }

  if (channels > 0)
    COR_I("%d channel%s configured", channels, (channels == 1) ? "" : "s");
}



// -----------------------------------------------------------------------------
//
// bridgesInit - open the transports, and hand each Channel to its Bridge
//
// LAST in startup: from here on, samples arrive on threads of the transports'
// own making, and everything one needs - the store, the caches, the hooks, and
// the Channels themselves - is already up.
//
static void bridgesInit(void)
{
  if (bridgeCount == 0)
    return;

  for (int i = 0; i < bridgeCount; i++)
  {
    BridgeDriver* driverP = &bridges[i];

    if (driverP->init == NULL)
      continue;

    if (driverP->init(bridgeConfig, &bridgeBroker) != BRIDGE_OK)
      COR_X(1, "init failed for bridge plugin '%s'", (driverP->alias != NULL) ? driverP->alias : "?");

    COR_I("bridge '%s' up%s%s",
          (driverP->alias != NULL) ? driverP->alias : "?",
          (driverP->versionInfo != NULL) ? ": " : "",
          (driverP->versionInfo != NULL) ? driverP->versionInfo() : "");
  }

  //
  // And only now is each Channel handed to its Bridge. A driver cannot
  // subscribe before its transport exists, so this cannot move above init() -
  // which is the whole reason the cache is filled first and the wire opened
  // last.
  //
  for (Channel* channelP = channelCacheFirst(); channelP != NULL; channelP = channelP->next)
  {
    if (channelP->status != ChannelStatusAvailable)
    {
      COR_I("channel '%s' is dormant: %s", channelP->endpoint,
            (channelP->statusReason != NULL) ? channelP->statusReason : "?");
      continue;
    }

    for (int i = 0; i < bridgeCount; i++)
    {
      if ((bridges[i].alias == NULL) || (strcmp(bridges[i].alias, channelP->bridgeName) != 0))
        continue;

      //
      // channelAddInfo (ABI 9) when the plugin has it - the Channel's channelInfo comes with it - else
      // channelAdd, and a channelInfo the plugin cannot be given is said, not silently dropped.
      //
      bool withInfo = (bridges[i].abiVersion >= 9) && (bridges[i].channelAddInfo != NULL);

      if ((withInfo == false) && (bridges[i].channelAdd == NULL))
        break;

      if ((withInfo == false) && (channelP->info != NULL))
        COR_W("bridge '%s' takes no channelInfo (ABI %d) - ignored for '%s'", channelP->bridgeName, bridges[i].abiVersion, channelP->endpoint);

      int r = (withInfo == true) ? bridges[i].channelAddInfo(channelP->endpoint, channelP->kind, channelP->direction, channelP->info)
                                 : bridges[i].channelAdd(channelP->endpoint, channelP->kind, channelP->direction);
      //
      // Refused - an address or a channelInfo the transport cannot use. The Channel stays, DORMANT, with the
      // reason: GET /channels is where an operator (and a test) looks, not the log.
      //
      if (r != BRIDGE_OK)
      {
        COR_W("bridge '%s' would not carry '%s' (%d)", channelP->bridgeName, channelP->endpoint, r);

        char reason[128];
        snprintf(reason, sizeof(reason), "the bridge would not carry it: %s",
                 (r == BRIDGE_BAD_INPUT)   ? "its address or channelInfo does not fit the transport" :
                 (r == BRIDGE_UNSUPPORTED) ? "the transport does not support this kind of channel"   : "refused");
        channelP->status       = ChannelStatusDormant;
        channelP->statusReason = strdup(reason);
      }

      break;
    }
  }
}



// -----------------------------------------------------------------------------
//
// bridgesClose - take every bridge's transport down
//
// ⭐ Must run BEFORE the DB plugin is closed, and for the same reason the
// periodic loop is stopped first: a bridge thread inside sampleIn is on its way
// into the store, and dbClose underneath it is a read of a pool that has
// already been destroyed. A driver's close() does not return until its threads
// have stopped, which is what makes the ordering sufficient.
//
static void bridgesClose(void)
{
  for (int i = 0; i < bridgeCount; i++)
  {
    if (bridges[i].close != NULL)
      bridges[i].close();
  }
}



// -----------------------------------------------------------------------------
//
// pernotQueryCallback - query entities for a periodic notification subscription
//
// Called by the pernot loop thread. Builds a DbQueryFilter from the
// pernot item's entity selectors and calls db.entityQuery.
//
// db.entityQuery (mongoc, corDB) allocates through corRest.kallocP.
// corRest is __thread; the pernot thread's copy is zero-initialised so we
// bring it up to working state on first call. Between pernot cycles we
// reset it — the produced entity array is cloned by the caller (through
// ldEntityToApi onto its own kaBuffer) before we return, so nothing
// outside this function holds pointers into corRest afterwards.
//
static CorNode* pernotQueryCallback(void* tenantP, LdPernotItem* itemP, void* allocP)
{
  if (db.entityQuery == NULL)
    return NULL;

  static __thread bool pernotCorRestInited = false;
  if (!pernotCorRestInited)
  {
    corAllocBufferInit(&corRest.kalloc, corRest.kallocBuffer, sizeof(corRest.kallocBuffer), 256 * 1024, NULL, "pernot");
    corRest.corJsonP = corJsonCreate(&corRest.corJson, &corRest.kalloc);
    corRest.kallocP  = &corRest.kalloc;
    pernotCorRestInited = true;
  }
  else
  {
    //
    // true = reuse. false frees the extra blocks and leaves the list that
    // holds them dangling, so the NEXT cycle frees them a second time - a
    // double free that only shows up once a cycle has outgrown the inline
    // buffer twice.
    //
    corAllocBufferReset(&corRest.kalloc, true);
  }

  //
  // One query per entity selector, the results merged without duplicates - a subscription's
  // entities[] is an OR of selectors, each an AND of its own type / id / idPattern, which one
  // filter cannot say. And the rest of the subscription's query - q, scopeQ, geoQ - on every one.
  //
  // It used to be one query, from the FIRST selector's type (and id) alone: the other selectors,
  // idPattern, scopeQ and geoQ were ignored (the last two were never even parsed), and it was
  // capped at 20 entities - a notification silently left the rest out. The selectors are read
  // from the stored subscription (the item is pinned by the loop), where their idPattern text is.
  //
  CorNode* entitiesP = corTreeLookup(itemP->subTree, LD_VOCAB_ENTITIES);
  CorNode* resultP   = NULL;

  for (CorNode* selP = (entitiesP != NULL) ? entitiesP->value.head : NULL; selP != NULL; selP = selP->next)
  {
    if (selP->type != CorObject)
      continue;

    CorNode* typeP      = corTreeLookup(selP, "type");
    CorNode* idP        = corTreeLookup(selP, "id");
    CorNode* idPatternP = corTreeLookup(selP, "idPattern");

    DbQueryFilter filter = {0};
    char*         typeV[2];
    char*         idV[2];

    if ((typeP != NULL) && (typeP->type == CorString))
    {
      if (strpbrk(typeP->value.s, "|&!(),;") != NULL)   // a type-selection expression (§ 4.17)
        filter.typeExpr = ldTypeExprParse(typeP->value.s, &corRest.kalloc);
      else
      {
        typeV[0] = typeP->value.s;
        typeV[1] = NULL;
        filter.typeV = typeV;
      }
    }

    if ((idP != NULL) && (idP->type == CorString))
    {
      idV[0] = idP->value.s;
      idV[1] = NULL;
      filter.idV = idV;
    }

    if ((idPatternP != NULL) && (idPatternP->type == CorString))
      filter.idPattern = idPatternP->value.s;

    filter.qExpr       = itemP->qExpr;
    filter.scopeExpr   = itemP->scopeExpr;
    filter.geoRel      = itemP->geoRel;
    filter.geometry    = itemP->geoGeometry;
    filter.coordinates = itemP->geoCoordinates;
    filter.geoproperty = itemP->geoProperty;
    filter.unpaged     = true;                     // every match - a notification is not a page

    CorNode* arrayP = NULL;
    if ((db.entityQuery((Tenant*) tenantP, &filter, &arrayP) != DB_OK) || (arrayP == NULL))
      continue;

    if (resultP == NULL)
    {
      resultP = arrayP;
      continue;
    }

    // Merge: an entity two selectors both match goes out once
    CorNode* nextP;
    for (CorNode* eP = arrayP->value.head; eP != NULL; eP = nextP)
    {
      nextP = eP->next;

      CorNode* eIdP = corTreeLookup(eP, "id");
      bool     dup  = false;

      if ((eIdP != NULL) && (eIdP->type == CorString))
      {
        for (CorNode* rP = resultP->value.head; (rP != NULL) && (dup == false); rP = rP->next)
        {
          CorNode* rIdP = corTreeLookup(rP, "id");
          dup = (rIdP != NULL) && (rIdP->type == CorString) && (strcmp(rIdP->value.s, eIdP->value.s) == 0);
        }
      }

      if (dup == false)
      {
        eP->next = NULL;
        corTreeChildAdd(resultP, eP);
      }
    }
  }

  return resultP;
}



// -----------------------------------------------------------------------------
//
// pernotTenantVisit - one tenant's pernot cache, and its Snapshots' own
//
#if COR_FEATURE_SNAPSHOT_SUBSCRIPTIONS
typedef struct PernotVisit
{
  LdPernotCacheVisitFn  visit;
  void*                 arg;
} PernotVisit;

//
// A Snapshot's own pernot cache: its notifications name the snapshot (NGSILD-Snapshot) - the item
// carries its tenant, the visit sets the snapshot. Pinned by snapshotTenantsVisit.
//
static void pernotSnapshotVisit(Tenant* snapTenantP, void* arg)
{
  PernotVisit* pvP = (PernotVisit*) arg;

  if (snapTenantP->pernotCacheP == NULL)
    return;

  corNgsild.snapshotId = snapTenantP->snapshotId;
  pvP->visit((LdPernotCache*) snapTenantP->pernotCacheP, pvP->arg);
  corNgsild.snapshotId = NULL;
}
#endif

static void pernotTenantVisit(Tenant* tP, LdPernotCacheVisitFn visit, void* arg)
{
  if (tP->pernotCacheP != NULL)
    visit((LdPernotCache*) tP->pernotCacheP, arg);

#if COR_FEATURE_SNAPSHOT_SUBSCRIPTIONS
  PernotVisit pv = { visit, arg };
  snapshotTenantsVisit(tP, pernotSnapshotVisit, &pv);
#endif
}



// -----------------------------------------------------------------------------
//
// brokerPernotCaches - every tenant's pernot cache, for the periodic loop
//
// The tenants: tenant0, then the list - published with a release store and never freed, so it
// is walked here without the tenant mutex, tenants created after startup included.
//
static void brokerPernotCaches(LdPernotCacheVisitFn visit, void* arg)
{
  pernotTenantVisit(&tenant0, visit, arg);

  for (Tenant* tP = __atomic_load_n(&tenantList, __ATOMIC_ACQUIRE); tP != NULL; tP = tP->next)
    pernotTenantVisit(tP, visit, arg);
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
// requirement itself is unsettled). From the SUBSCRIPTION's tenant - it was tenant0's,
// whatever the subscription's.
//
static CorNode* throttleRetrieveCallback(void* tenantP, const char* entityId, void* allocP)
{
  (void) allocP;
  if (db.entityRetrieve == NULL)
    return NULL;

  CorNode* entityP = NULL;
  if (db.entityRetrieve((tenantP != NULL) ? (Tenant*) tenantP : &tenant0, entityId, &entityP) != DB_OK)
    return NULL;

  return entityP;
}



// -----------------------------------------------------------------------------
//
// brokerTenantCaches - every tenant's caches, for the periodic ticks (throttle flush, CSR-subs)
//
// tenant0, then the list - published with a release store and never freed, so it is walked
// without the tenant mutex, tenants created after startup included.
//
static void tenantCachesVisit(Tenant* tP, LdTenantCachesVisitFn visit, void* arg)
{
  LdTenantCaches tc = { tP, (LdSubCache*) tP->subCacheP, (LdSubCache*) tP->regSubCacheP, (LdRegCache*) tP->regCacheP };

  //
  // The tick runs AS the visited tenant, as a request runs as its own: what it sends goes out
  // tagged with it (NGSILD-Tenant - and NGSILD-Snapshot for a Snapshot's own tenant), and it has
  // no request to take the tenant from.
  //
  corNgsild.tenantP    = tP;
  corNgsild.tenantName = tenantApiName(tP);
  corNgsild.snapshotId = tP->snapshotId;

  visit(&tc, arg);

  corNgsild.tenantP    = NULL;
  corNgsild.tenantName = NULL;
  corNgsild.snapshotId = NULL;
}

#if COR_FEATURE_SNAPSHOT_SUBSCRIPTIONS
typedef struct TenantCachesVisit
{
  LdTenantCachesVisitFn  visit;
  void*                  arg;
} TenantCachesVisit;

static void tenantCachesSnapshotVisit(Tenant* snapTenantP, void* arg)
{
  TenantCachesVisit* tvP = (TenantCachesVisit*) arg;

  tenantCachesVisit(snapTenantP, tvP->visit, tvP->arg);
}
#endif

static void brokerTenantCaches(LdTenantCachesVisitFn visit, void* arg)
{
#if COR_FEATURE_SNAPSHOT_SUBSCRIPTIONS
  TenantCachesVisit tv = { visit, arg };
#endif

  for (Tenant* tP = &tenant0; tP != NULL; tP = (tP == &tenant0)? __atomic_load_n(&tenantList, __ATOMIC_ACQUIRE) : tP->next)
  {
    tenantCachesVisit(tP, visit, arg);
#if COR_FEATURE_SNAPSHOT_SUBSCRIPTIONS
    snapshotTenantsVisit(tP, tenantCachesSnapshotVisit, &tv);   // the subscriptions on its Snapshots (throttle flush)
#endif
  }
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
  // Over the memory budget: refused before anything is resolved or downloaded for it (memoryBudget.h)
  if (memoryBudgetAdmit() == false)
    return false;

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
  //
  // It answers false when the request NAMED an @context that could not be
  // retrieved. It does not raise the error itself - only here is it known
  // whether the operation uses the context at all - so the 504 is raised here,
  // for GET and HEAD, whose response body is compacted against it. Carrying on
  // would compact against core and advertise core in the response Link header:
  // self-consistent, wrong, and silent.
  //
  // Scope: GET and HEAD, whose response body is compacted against the context.
  // A request that named a context we cannot fetch cannot be answered honestly
  // there - and answering it against core instead, while advertising core, is
  // what this fixes.
  //
  // ⚠️ Deliberately NOT wider, though wider is arguably more correct.
  // entity_delete_with_unresolvable_context asserts that DELETE /entities/{id}
  // with an undownloadable Link @context SUCCEEDS: no body either way and the id
  // is already a full URI. That test cites no clause - it is a decision of ours,
  // not a spec rule - and widening this would silently change it.
  //
  // ❓ OPEN, with ETSI TC DATA (asked 2026-09-17), because the spec does not say
  // and TP 043_01 covers only the five CREATE cases - nothing for reads or
  // deletes. Three positions: (a) silently ignore a context the operation cannot
  // use, (c) 400 for naming one an endpoint never uses, (b) always fetch and
  // fail. What the answer has to settle first is whether the requirement is a
  // property of the ENDPOINT or of the REQUEST, because these all use the
  // context invisibly and none of them is a GET:
  //
  //   DELETE /entities/{id}?type=T          - `type` is expanded (registration matching)
  //   DELETE /entities/{id}/attrs/{attr}    - the attribute name is expanded
  //   DELETE /entities (purge)              - carries many name-carrying params
  //
  // and one goes the other way: POST /entityOperations/delete is 504'd by the
  // parse hook today, though its body is an array of ids with nothing to expand.
  //
  // A per-REQUEST rule (reject iff the context is really used) is precise and
  // makes one endpoint's contract depend on which params came with the call. A
  // per-ENDPOINT rule is predictable and over-rejects. Not guessing: the narrow
  // fix lands, the rest waits for the answer.
  //
  // If corJsonld's error callback already named WHY (a cyclic @context, say),
  // contextError is set and that answer stands - it is one the client can act
  // on, where "unable to retrieve" is vaguer and wrong.
  //
  bool contextOk = ldContextResolve();

  //
  // An error corNgsild has already RAISED is unconditional, whatever the verb:
  // a malformed Link header is a malformed request even for an operation that
  // would never have used the @context, and a cyclic @context has already been
  // named more precisely than anything decided here could name it.
  //
  // Checked explicitly rather than left to fall out of the 504 branch below.
  // It did fall out - ldError sets the response error immediately, so the 400
  // stood anyway - but only by accident, and an accident is not a rule.
  //
  if (corNgsild.contextError == true)
    return false;

  //
  // Retrievability is the conditional half: the @context was named and could
  // not be fetched, which only matters if this request uses it.
  //
  if (!contextOk && (corRest.in.verb == CorVerbGet || corRest.in.verb == CorVerbHead))
  {
    ldError(504, LD_ERROR_LD_CONTEXT_NOT_AVAILABLE, "Context Not Available",
            "unable to retrieve @context from '%s'", corNgsild.contextUnavailableUrl);
    corNgsild.contextError = true;
    return false;
  }

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
  bridgeRequestsReleasePending();     // after the notifications - a goal's events must not overtake them
  dbExpiredEntityDispatchPending();   // transient Entities a read found expired
  ldRegCacheProbePending();
  ldSubEntityTypeExprsRelease();   // free the per-request subscription type-expr scratch
  ldSnapshotRequestRelease();      // the Snapshot a read was routed to (NGSILD-Snapshot), pinned till now
  ldEntityMapRequestRelease();     // the EntityMap the request created or paged, pinned till now
#if COR_FEATURE_HEALTH
  healthRequestEnd();              // last: a request stuck in its post-response work is still in flight
#endif
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
static void brokerLinkedEntitiesHook(CorNode* dataArrayP, const char* mode, int joinLevel, bool sysAttrs, void* tenantP)
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
// right after; corTreeClone(NULL, parsed) deep-copies into malloc-backed
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
      COR_X(1, "--contextSourceExtras: cannot open '%s'", path);
    return;
  }

  fseek(fp, 0, SEEK_END);
  long fsz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (fsz <= 0 || fsz > 1024 * 1024)
  {
    fclose(fp);
    COR_X(1, "contextSourceExtras: '%s' empty or too large (max 1 MiB)", path);
  }

  char* buf = (char*) corAlloc(&corRest.kalloc, fsz + 1);
  if (fread(buf, 1, fsz, fp) != (size_t) fsz)
  {
    fclose(fp);
    COR_X(1, "contextSourceExtras: read failed on '%s'", path);
  }
  fclose(fp);
  buf[fsz] = 0;

  CorNode* parsed = corJsonParse(corRest.corJsonP, buf);
  if (parsed == NULL)
    COR_X(1, "contextSourceExtras: '%s' is not valid JSON", path);

  ldContextSourceExtras = corTreeClone(NULL, parsed);
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

  crashReportInstall(argC, argV);

  //
  // --version is answered before kargs is initialized and before any plugin is
  // loaded: asking a binary what it is must not depend on a plugin directory
  // being present, nor on a DB plugin being loadable. It is the one question a
  // broken installation still has to be able to answer.
  //
  // corArgsPeek is what makes that possible without hand-parsing argV - it reads
  // the option table directly, answers "SET" for a CorArgBool, and matches the short
  // name too. It is the same mechanism the plugin peek below uses.
  //
  if (corArgsPeek(argC, argV, kargV, "--version") != NULL)
  {
    printf("%s %s\n", progName, CORAINE_VERSION);

    //
    // The feature set belongs here for the same reason the version does: it is
    // a property of the BINARY, not of a running broker, and a build with an
    // endpoint compiled out looks identical from the outside until asked. Every
    // feature is listed with its value - a missing name would be ambiguous
    // between "off" and "this build is too old to know the name".
    //
    printf("features:");
    for (int ix = 0; coraineFeatures[ix].name != NULL; ix++)
      printf(" %s=%d", coraineFeatures[ix].name, coraineFeatures[ix].on ? 1 : 0);
    printf("\n");

    //
    // The HTTP server, on a line of its own and NOT in the feature set above.
    // Those are booleans - is this capability in the build - and this is a
    // choice between two implementations of a capability that is always in it.
    // Folding it in would mean inventing a feature name whose "off" meant
    // nothing, and it is also read differently: a test file asks
    // REQUIRE_HTTPSERVER for one current value out of alternatives, where
    // REQUIRE_FEATURE asks for membership of a set.
    //
    // GET /build reports the same thing to a RUNNING broker; this line is what
    // answers before one has started, which is when the test harness asks.
    //
    printf("httpServer: %s\n", CORAINE_HTTP_SERVER);

    exit(0);
  }

  CorArgsStatus ks = corArgsInit(progName, kargV, "CORAINE");
  if (ks != CorArgsOk)
    COR_X(1, "corArgsInit failed: %s", corArgsStatus(ks));

  corPluginSetBaseDir("/opt/seamware/plugins", "SEAMWARE_PLUGIN_DIR");

  bool startupError = pluginsLoad(argC, argV);

  ks = corArgsParse(argC, argV);
  if (ks != CorArgsOk)
    COR_X(1, "corArgsParse failed: %s", corArgsStatus(ks));

  if (startupError)
  {
    corArgsUsage();
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


  startupLog("coraine", traceLevels);   // the log, and the libraries' log through it - as coraine-import's

  COR_V("coraine  %s", CORAINE_VERSION);
  COR_I("Advertised HTTP endpoint: %s (%s)", ldBrokerHttpEndpoint, endpointSource);

  sem_init(&shutdownSem, 0, 0);
  signal(SIGINT,  onSignal);
  signal(SIGTERM, onSignal);

  // User-Agent uses the <product>/<version> form — a bare
  // product name (e.g. "coraine") is blocked by some @context CDNs
  // (uri.etsi.org via Cloudflare) but slash-versioned tokens pass.
  if (corRestClientInit(4, 60, "Coraine/" CORAINE_VERSION) != 0)
    COR_X(1, "corRestClientInit failed");

  // --insecureNotif → notifications/forwards to TLS endpoints accept self-signed
  // certificates (set before the first TLS handshake, i.e. before corRestClientTlsInit)
  if (insecureNotif)
    corRestClientTlsInsecureSet(true);

  //
  // Notifications to anything but HTTP - mqtt://, mqtts:// - go through the bridge plugin claiming the
  // scheme (--bridges mqtt), and --insecureNotif goes with them for mqtts://
  //
  bridgeNotifyInit(insecureNotif);

  static CorAlloc  contextAlloc;
  static char      contextBuffer[64 * 1024];

  // allocSize must be non-zero — corAlloc falls back to calloc(1, allocSize)
  // when the static 64 KiB initial buffer runs out, and calloc(1, 0) returns
  // NULL/empty, leading to a SEGV on the next memset. 256 KiB matches the
  // pernot/corRest convention for "ample headroom for normal growth".
  corAllocBufferInit(&contextAlloc, contextBuffer, sizeof(contextBuffer), 256 * 1024, NULL, "jsonld-context");

  //
  // The context store is SHARED: every thread that downloads or parses an @context allocates
  // into it, outside the context cache's own mutex. Two different uncached @contexts arriving
  // at once raced on its allocation pointer - overlapping allocations, a corrupted block list.
  //
  corAllocThreadSafe(&contextAlloc);

  corLdConcurrencySet(contextOwner, contextSleep);

  //
  // The core context with every term the broker adds to it, the CorTerm ids, the NGSI-LD library -
  // as coraine-import sets them up
  //
  const char* coreContextError = startupCoreContext(&contextAlloc);
  if (coreContextError != NULL)
    COR_X(1, "%s", coreContextError);

  ldDefaultCooldownNs = (uint64_t) cooldownMillis * 1000000ULL;

#if COR_FEATURE_REGISTRATIONS
  forwardingHttpRegister();
  forwardingCorRegister();
#endif

  //
  // cor:// - the codec both directions use, as a client (forwarding) and as a server (--corPort)
  //
  corRestCorInit(&ldBinCodec, ldBinNamespaceV, ldBinNamespaces, CorTermLast);

  if (prettySpaces > 0)
    corRestSetPrettySpaces(prettySpaces);

  // § 6.3.2 413 threshold. 0 in --maxRequestSize disables the cap.
  corRestSetMaxRequestSize(((unsigned long long) maxRequestSize) * 1024ULL * 1024ULL);

  if (corsOrigin != NULL)
  {
    const char* origin = (strcmp(corsOrigin, "__ALL") == 0) ? "*" : corsOrigin;

    CorRestCorsConfig corsConf = {
      .allowOrigin   = origin,
      .allowHeaders  = "Content-Type, Accept, Link, NGSILD-Tenant, NGSILD-Path, Authorization, Service-Execution",
      .exposeHeaders = "Location, NGSILD-Results-Count, Link, NGSILD-Tenant, NGSILD-Warning, Service-Execution",
      .maxAge        = corsMaxAge
    };
    corRestCorsConfig(&corsConf);
  }

  if (corRestParamAdd(bridgeParams) == false)
    COR_X(1, "corRestParamAdd failed for the broker's own URL parameters");

  if (corRestParamAdd(pageParams) == false)
    COR_X(1, "corRestParamAdd failed for the page position parameters");

#if COR_FEATURE_SERVICE_EXECUTION
  if (corRestParamAdd(serviceParams) == false)
    COR_X(1, "corRestParamAdd failed for the Service Execution URL parameters");
#endif

  apiPluginsInit();
  //
  // Before any snapshot is loaded: a snapshot's stores and tenant are destroyed by whoever
  // releases its last reference (see snapshotItemDestroy), which is not always the DELETE.
  //
  ldSnapshotCacheDestroyHookSet(snapshotItemDestroy);

  tenantInit("cor");
  metricsInit();
  ldNotifyStatsHookSet(brokerNotifyStatsHook);
  ldLinkedEntitiesHookSet(brokerLinkedEntitiesHook);
  corRestSetServiceInitHook(ldUrlWildcardOptionsInit);
  corRestSetPreServiceHook(brokerPreServiceHook);
  corRestSetPostResponseHook(brokerPostResponseHook);
  inlineDispatchInit(dbName, troeName, noInline);
  memoryBudgetInit(memoryLimit);
  metricsMemoryValuesSet(memoryBudgetValues);

  //
  // The memory of the EntityMaps (ldEntityMap.h) - automatic ones and the ones clients ask for
  //
  ldEntityMapMaxBytes = ((int64_t) entityMapMemory) * 1024 * 1024;
  COR_V("EntityMap memory: %d MiB%s", entityMapMemory, (entityMapMemory == 0)? " (no cap, no automatic EntityMaps)" : "");

#if COR_FEATURE_AUTO_ENTITY_MAP
  //
  // Which queries get an automatic EntityMap (getEntities.h): the distributed ones by default - a local
  // one pages by offset/limit in the store, cheaper than a map (doc/installation.md#entitymaps)
  //
  if      (strcmp(autoEntityMapsArg, "none")        == 0) autoEntityMaps = AutoEntityMapsNone;
  else if (strcmp(autoEntityMapsArg, "distributed") == 0) autoEntityMaps = AutoEntityMapsDistributed;
  else if (strcmp(autoEntityMapsArg, "all")         == 0) autoEntityMaps = AutoEntityMapsAll;
  else
    COR_X(1, "--autoEntityMaps: '%s' is none of none, distributed, all", autoEntityMapsArg);

  COR_V("automatic EntityMaps: %s", autoEntityMapsArg);
#endif

#if COR_FEATURE_RESPONSE_BUDGET
  //
  // The byte budget of an entity query (responseBudget.h): --maxResponseSize when given (0: none),
  // else 1/16 of the memory budget - a container's limit or --memoryLimit - and none without one. A
  // response holds more than its bytes (the fetched entities, the rendered body, its send buffer)
  // and several run at once, so a sixteenth leaves the memory budget room for them.
  //
  if (maxResponseSize >= 0)
    responseBudgetBytes = ((int64_t) maxResponseSize) * 1024 * 1024;
  else
  {
    uint64_t budget, used, resident, refused;

    memoryBudgetValues(&budget, &used, &resident, &refused);
    responseBudgetBytes = (int64_t) (budget / 16);
  }

  if (responseBudgetBytes > 0)
    COR_V("response size budget: %lld MiB (%s)", (long long) (responseBudgetBytes >> 20),
          (maxResponseSize >= 0)? "--maxResponseSize" : "1/16 of the memory budget");
  else
    COR_V("response size budget: none");
#endif

#if COR_FEATURE_HEALTH
  //
  // The health port BEFORE the store loads: /live answers through a long load (a persistent corDB),
  // /ready only once the broker serves (healthServing, below)
  //
  if ((healthPort != 0) && (healthStart(healthPort, healthStallTimeout) == false))
    COR_X(1, "cannot listen for the health probes on port %u", healthPort);
#endif

  if (dbStart() != 0)
    COR_X(1, "dbStart failed");

#if COR_FEATURE_HEALTH
  healthStoreLoaded();
#endif

  if (troeStart() != 0)
    COR_X(1, "troeStart failed");

#if COR_FEATURE_HEALTH
  healthTroeLoaded();
#endif

  //
  // Load subscriptions from DB into cache.
  // mongoc uses corRest.kalloc internally — set up a startup buffer for it.
  // Cache items get cloned into persistent (malloc) storage, so this is short-lived.
  //
  startupArena();

  //
  // "Now", for anything written before the first request arrives.
  //
  // corRest.requestStartTime is what stamps createdAt/modifiedAt, and the HTTP
  // backends set it as each request lands. Nothing sets it before that, so
  // every write the broker performs during startup - pre-populated channel
  // entities today, whatever else later - is stamped zero and reads back as
  // 1970 for the life of the deployment. Startup is a perfectly good moment to
  // know what time it is.
  //
  {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    corRest.requestStartTime = (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
  }
  //
  // --ha: from here on, what another broker instance writes reaches our caches
  // too. BEFORE the reload below, not after - see haInit.h: listening only after
  // reading the database leaves a window whose changes are missed for good.
  // Nothing is applied until haApplyEnable() below.
  //
  if (haInit() == false)
    COR_X(1, "unable to start the HA channel '%s'", haChannel);

  tenantSubCacheReload();
  tenantRegCacheReload();
  tenantSnapshotCacheReload();
  contextCacheReload();

  haApplyEnable();

  contextSourceExtrasLoad(contextSourceExtras);

  //
  // Channels and their placeholder entities, while corRest.kalloc is still a
  // live arena - see bridgeChannelsInit. The transports themselves come up much
  // later, once everything else is running.
  //
  bridgeChannelsInit();
  bridgeSyncTimeoutSettle();   // --ddsSyncTimeout, else the bridge configuration's syncTimeoutMs, else 200

  corAllocBufferReset(&corRest.kalloc, false);

  // Register the pernot subsystem with the shared periodic-dispatch
  // engine. The engine itself is launched once below.
  ldPernotLoopStart(brokerPernotCaches, pernotQueryCallback);   // every tenant, not only tenant0

  // § 5.2.x throttling — register the coalesce-to-latest flush (sole sender for
  // throttled subs; the synchronous path only buffers into the dirty set).
  ldThrottleFlushStart(brokerTenantCaches, throttleRetrieveCallback);   // every tenant, not only tenant0

  // § 5.11.7 — register the CSR-Sub periodic ticker. Skips items with
  // timeInterval == 0 (change-driven) by design.
  ldCsrSubPeriodicLoopRegister(brokerTenantCaches);   // every tenant, not only tenant0

  // Register the volatile-context reaper — drops never-fetched one-shot
  // hosted contexts (response / forward Link targets) past their TTL.
  ldContextHostReaperStart();

#if COR_FEATURE_SERVICE_EXECUTION
  // Service Executions: time-outs (failed) and retention (deleted), every tenant
  ldPeriodicLoopRegister(seExecutionTick, NULL);

  // ... and the reports of the bridges that execute services (the broker as Service Executor)
  bridgeServiceApplySet(seExecutionApplyBridge);
#endif

  // Start the shared periodic dispatch thread (1-Hz tick over all
  // registered consumers).
  ldPeriodicLoopStart();

  // Start the sub-stats periodic flush loop. --subStatsFlushInterval
  // defaults to 60s; 0 disables. The admin endpoint remains available
  // regardless.
  ldStatsFlushLoopStart(subStatsFlushInterval, subStatsFlushAll);

  bridgesInit();

  //
  // Transport plugins (--transports): the API over another protocol - a WebSocket upgraded from
  // GET /ngsi-ld/v1/ws (doc/websocket.md). Before the REST server starts: the upgrade hook is set here.
  //
#if COR_FEATURE_TRANSPORTS
  if (transportNames != NULL)
  {
    char errBuf[1024] = "";

    if ((transportLoad(transportNames, errBuf, sizeof(errBuf)) != 0) || (transportInit() == false))
    {
      fprintf(stderr, "%s\n", (errBuf[0] != 0) ? errBuf : "a transport plugin did not start - see the log");
      exit(1);
    }
  }
#endif

  //
  // Build combined service array (core + plugins) and start the REST server
  //
  int totalServices = 0;
  CorRestServiceSimplified* allServices = serviceBuild(&totalServices);
  if (allServices == NULL)
    COR_X(1, "serviceBuild failed (out of memory)");

  //
  // ?ddsSync on EVERY route, as Orion-LD takes it: a client that sends it
  // everywhere is not refused (400) where there is nothing to wait for. Only
  // the three entity PATCH forms act on it - see bridgeServiceSync.h. A route
  // that takes any parameter at all (~0) needs nothing.
  //
  for (int ix = 0; ix < totalServices; ix++)
  {
    if (allServices[ix].supportedParams != ~(uint64_t) 0)
      allServices[ix].supportedParams |= BRIDGE_PARAM_DDS_SYNC;
  }

  //
  // How many event loops the built-in server runs. A no-op on a libmicrohttpd
  // build, which has a thread per connection and no loop of ours to multiply -
  // the option is accepted either way so that a deployment does not have to
  // know which server its binary carries.
  //
  // Four by default, measured on 8 cores with 319-byte responses:
  //
  //   loops   req/s     p99       cores
  //     1     72 054    970 us     1.8
  //     2    135 189    587 us     3.6
  //     4    210 496    437 us     7.1
  //     8    245 271    3.21 ms    8.0
  //
  // Eight buys 16% more throughput for SEVEN TIMES the tail, because at 245k
  // the box is saturated and the queue is what is being measured. Four scales
  // near-linearly, keeps the tail under half a millisecond and leaves headroom.
  //
  // So: one loop per core, capped at four. The cap is the table above; the
  // per-core part is because four loops on one core is four loops competing
  // for it, which measured SLOWER than one (6 211 req/s against 7 199). A
  // container with --cpus=1 is a normal deployment and must not pay for that.
  //
  // corCpuCount() is what the process may actually run on: its CPU affinity
  // intersected with any cgroup quota, not the host's core count, which is
  // what every "nproc" in a container gets wrong.
  //
  // A deployment that wants the saturation peak, or one loop, says so with
  // --httpLoops; only 0 asks us to decide.
  //
  if (httpLoops == 0)
  {
    httpLoops = corCpuCount();
    if (httpLoops > 4)
      httpLoops = 4;
  }

  corRestHttpLoopsSet(httpLoops);

  //
  // The worker pool. A request that waits - on mongod, on a forwarded request - holds its worker for the
  // wait, so a store that waits needs more workers than cores: 32. One that never waits (corDB, ramDB, and
  // no distributed operations) keeps every worker busy, and more of them than cores is the kernel taking
  // them off the CPU in turn: 32 on 8 cores was 74 000 involuntary context switches a second and a p95 of
  // 0.81 ms on a retrieve; 8 was 522 a second, 0.20 ms, and 6 % more requests (doc/history/performance.md).
  //
  if (poolSize == 0)
    poolSize = ((inlineDispatchNeverWaits() == true) && (distributed == false)) ? corCpuCount() : 32;

  if (corRestInit(allServices, totalServices, (unsigned short) port, poolSize) != 0)
    COR_X(1, "corRestInit failed on port %u", port);

  COR_I("coraine running on port %u", port);

  //
  // cor:// last, like HTTP: every service is registered and every cache loaded by now
  //
  if ((corPort != 0) && (corRestCorListen(corPort, httpLoops) == false))
    COR_X(1, "cannot listen for cor:// on port %u", corPort);

#if COR_FEATURE_HEALTH
  healthServing();   // every cache loaded, every port open: /ready may answer 200
#endif

  // Until SIGINT / SIGTERM (onSignal) - sem_wait returns early on EINTR, so wait again
  while (sem_wait(&shutdownSem) != 0)
    ;

  shutdownInOrder();

  return 0;
}

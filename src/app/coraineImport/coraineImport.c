//
// FILE            coraineImport.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// coraine-import - the writer half of a migration (doc/migration.md): a migration stream written into
// the stores the options name, then exit.
//
//   coraine-import --database corDB --dbDir /var/lib/coraine --troe corDB --file stream.ndjson
//
// A program of its own, not a mode of the broker: the broker carries no import code, and the importer
// serves nothing. It is linked against the broker's libraries and loads the broker's store plugins,
// with the broker's option names (--database, --troe and each plugin's own: --dbDir, --dbHost,
// --troeName, ...), so a record is written by the code a live write goes through, into exactly what a
// broker started with the same store options reads.
//
#include <stdbool.h>                                       // bool
#include <stdio.h>                                         // printf, fprintf
#include <stdlib.h>                                        // exit, malloc
#include <string.h>                                        // strrchr, memcpy
#include <time.h>                                          // time

#include "corAlloc/corAlloc.h"                             // CorAlloc
#include "corAlloc/corAllocBufferInit.h"                   // corAllocBufferInit
#include "corLog/corLog.h"                                 // COR_X
#include "corLog/corLogGlobals.h"                          // corLogInfo, corLogVerbose, corLogDebug
#include "corLog/corLogOut.h"                              // corLogOut
#include "corBase/corBaseInit.h"                           // corBaseInit, corBaseTraceLevelsSet
#include "corArgs/corArgs.h"                               // corArgsInit, corArgsParse, corArgsPeek, CorArg, CORARGS_END
#include "corPlugin/corPlugin.h"                           // corPluginSetBaseDir, corPluginArgUpdate
#include "corRest/corRestClient.h"                         // corRestClientInit, CorRestClientRequest/Response
#include "corRest/CorRestState.h"                          // corRest
#include "corJson/corJsonCreate.h"                         // corJsonCreate
#include "corJsonld/corJsonld.h"                           // corLdInit

#include "corNgsild/corNgsild.h"                           // ldInit
#include "corNgsild/CorNgsild.h"                           // corNgsild, ldDistributed, ldBrokerStartTimeSec
#include "corNgsild/ldCoreTermIds.h"                       // ldCoreTermIdsInit
#include "corNgsild/ldExtensionTerms.h"                    // ldExtensionTermsAdd
#include "corNgsild/ldError.h"                             // ldError
#include "corNgsild/LdProblem.h"                           // LD_ERROR_BAD_REQUEST_DATA, LD_ERROR_LD_CONTEXT_NOT_AVAILABLE

#include "db/DbDriver.h"                                   // db
#include "db/dbInit.h"                                     // dbStart
#include "db/dbClose.h"                                    // dbClose
#include "db/Tenant.h"                                     // tenantInit, tenantSubCacheReload, tenantRegCacheReload
#include "db/contextCache.h"                               // contextCacheReload
#include "troe/TroeDriver.h"                               // troe
#include "troe/troeInit.h"                                 // troeStart, troeStop
#include "plugin/pluginLoader.h"                           // pluginLoadDb, pluginLoadTroe, pluginTroeArgUpdate
#include "bridge/bridgeCoreTerms.h"                        // bridgeCoreTermsAdd
#if COR_FEATURE_SERVICE_EXECUTION
#include "serviceExecution/seCoreTerms.h"                  // seCoreTermsAdd
#include "corNgsild/ldServiceDescription.h"                // ldServiceDescriptionAccepted
#endif

#include "migrate/migrateImport.h"                         // migrateImport

#include "../coraine/coraineVersion.h"                     // CORAINE_VERSION



// -----------------------------------------------------------------------------
//
// Command line arguments - the broker's names for what a store needs; the plugins add their own
//
static char* dbName      = "mongoc";
static char* troeName    = "none";
static char* file        = NULL;
static char* traceLevels = NULL;
static bool  versionOnly = false;

//
// The broker's libraries are linked whole (the plugins resolve against them): what they read of the
// broker's own options is defined here, at the value of a broker that has it unset
//
bool asyncSnapshot = false;                                // --asyncSnapshot - the importer takes no Snapshot

#define _vp (void*)
static CorArg kargV[] =
{
  { "--file",        "-f",     CorArgString, _vp &file,        CorArgReq, _vp NULL,     NULL,      NULL,      "the migration stream: one JSON record per line, expanded NGSI-LD ('-' = stdin)" },
  { "--database",    "-db",    CorArgString, _vp &dbName,      CorArgOpt, _vp "mongoc", NULL,      NULL,      "database plugin (short name or full path) - as the broker's" },
  { "--troe",        "-troe",  CorArgString, _vp &troeName,    CorArgOpt, _vp "none",   NULL,      NULL,      "TRoE temporal-storage plugin (short name or full path; 'none': no history imported) - as the broker's" },
  { "--traceLevels", "-t",     CorArgString, _vp &traceLevels, CorArgOpt, _vp NULL,     NULL,      NULL,      "trace levels" },
  { "--version",     "-V",     CorArgBool,   _vp &versionOnly, CorArgOpt, _vp false,    _vp false, _vp true,  "print version and exit" },
  CORARGS_END
};
#undef _vp



// -----------------------------------------------------------------------------
//
// contextDownload - CorLdDownloadFunction: a subscription's jsonldContext is fetched when it is created
//
static char* contextDownload(const char* url, int* statusCodeP)
{
  CorRestClientRequest  req;
  CorRestClientResponse resp;

  corRestClientRequestInit(&req, CorVerbGet, url, NULL);
  corRestClientRequestHeader(&req, "Accept", "application/ld+json, application/json");
  corRestClientRequestTimeout(&req, 5000, 10000);

  if ((corRestClientSend(&req, &resp) != CORR_OK) || (resp.statusCode != 200))
  {
    *statusCodeP = (resp.statusCode > 0) ? resp.statusCode : 500;
    corRestClientResponseCleanup(&resp);
    return NULL;
  }

  *statusCodeP = 200;

  char* copy = NULL;                                       // malloc - corJsonld frees it
  if ((resp.body != NULL) && (resp.bodyLen > 0) && ((copy = (char*) malloc(resp.bodyLen + 1)) != NULL))
  {
    memcpy(copy, resp.body, resp.bodyLen);
    copy[resp.bodyLen] = 0;
  }

  corRestClientResponseCleanup(&resp);
  return copy;
}



// -----------------------------------------------------------------------------
//
// contextError - CorLdErrorFunction: an @context the library can name - the record's refusal says it
//
static void contextError(int status, const char* title, const char* detail)
{
  ldError(status, (status == 400) ? LD_ERROR_BAD_REQUEST_DATA : LD_ERROR_LD_CONTEXT_NOT_AVAILABLE, title, "%s", detail);
  corNgsild.contextError = true;
}



// -----------------------------------------------------------------------------
//
// pluginArgsAdd - a plugin's options into the table, under a separator naming the plugin
//
static void pluginArgsAdd(const char* kind, const char* alias, CorArg* argV)
{
  static char    sepText[2][128];
  static CorArg  sepArgV[2][2];
  static int     sepN = 0;

  if ((argV == NULL) || (sepN >= 2))
    return;

  snprintf(sepText[sepN], sizeof(sepText[sepN]), "%s (%s) plugin options:", kind, (alias != NULL) ? alias : "?");
  sepArgV[sepN][0]             = (CorArg) CORARGS_SEPARATOR(NULL);
  sepArgV[sepN][1]             = (CorArg) CORARGS_END;
  sepArgV[sepN][0].description = sepText[sepN];

  if ((corArgsAdd(sepArgV[sepN]) != CorArgsOk) || (corArgsAdd(argV) != CorArgsOk))
    COR_X(1, "the %s plugin's options could not be added", kind);

  ++sepN;
}



// -----------------------------------------------------------------------------
//
// pluginsLoad - the store plugins, before the options are parsed: they bring options of their own
//
static bool pluginsLoad(int argC, char* argV[])
{
  char* dbPeek   = corArgsPeek(argC, argV, kargV, "--database");
  char* troePeek = corArgsPeek(argC, argV, kargV, "--troe");
  char  errBuf[1024];
  bool  error    = false;

  if (pluginLoadDb((dbPeek != NULL) ? dbPeek : dbName, errBuf, sizeof(errBuf)) != 0)
  {
    fprintf(stderr, "%s\n", errBuf);
    error = true;
  }
  else
    pluginArgsAdd("Database", db.alias, db.args);

  if (pluginLoadTroe((troePeek != NULL) ? troePeek : troeName, errBuf, sizeof(errBuf)) != 0)
  {
    fprintf(stderr, "%s\n", errBuf);
    error = true;
  }
  else
    pluginArgsAdd("TRoE", troe.alias, troe.args);

  corPluginArgUpdate("--database", "db/currentState");
  pluginTroeArgUpdate();

  return error;
}



// -----------------------------------------------------------------------------
//
// main -
//
int main(int argC, char* argV[])
{
  char* progName = strrchr(argV[0], '/');
  progName = (progName != NULL) ? progName + 1 : argV[0];

  if (corArgsPeek(argC, argV, kargV, "--version") != NULL)
  {
    printf("%s %s\n", progName, CORAINE_VERSION);
    exit(0);
  }

  if (corArgsInit(progName, kargV, "CORAINE") != CorArgsOk)
    COR_X(1, "corArgsInit failed");

  corPluginSetBaseDir("/opt/seamware/plugins", "SEAMWARE_PLUGIN_DIR");

  bool startupError = pluginsLoad(argC, argV);

  CorArgsStatus ks = corArgsParse(argC, argV);
  if (ks != CorArgsOk)
    COR_X(1, "corArgsParse failed: %s", corArgsStatus(ks));

  if (startupError)
  {
    corArgsUsage();
    exit(1);
  }

  //
  // The log is stdout (traces, the plugins' lines); the import's report is stderr
  //
  if (corLogInit(progName, NULL, true, NULL, traceLevels, corArgsBuiltinVerbose, corArgsBuiltinDebug, false) != 0)
    COR_X(1, "corLogInit failed");

  corBaseInit(corLogOut);
  corBaseTraceLevelsSet(corLogTraceLevels, sizeof(corLogTraceLevels) / sizeof(corLogTraceLevels[0]));
  corLogInfo    = corArgsBuiltinVerbose;
  corLogVerbose = corArgsBuiltinVerbose;
  corLogDebug   = corArgsBuiltinDebug;

  //
  // An import stores, it never forwards nor notifies: no registration is asked, whatever is registered
  //
  ldDistributed        = false;
  ldBrokerStartTimeSec = (long long) time(NULL);

  if (corRestClientInit(4, 60, "Coraine/" CORAINE_VERSION) != 0)
    COR_X(1, "corRestClientInit failed");

  //
  // The core context, with every term the broker adds to it - an expanded stream may hold them short
  //
  static CorAlloc  contextAlloc;
  static char      contextBuffer[64 * 1024];

  corAllocBufferInit(&contextAlloc, contextBuffer, sizeof(contextBuffer), 256 * 1024, NULL, "jsonld-context");

  if (corLdInit(&contextAlloc, NULL, contextDownload, contextError) != 0)
    COR_X(1, "corLdInit failed");

  if (bridgeCoreTermsAdd(&contextAlloc) != 0)
    COR_X(1, "the ContextBridge/Channel terms could not be added to the core context");

  if (ldExtensionTermsAdd(&contextAlloc) != 0)
    COR_X(1, "the NGSI-LD extension terms could not be added to the core context");

#if COR_FEATURE_SERVICE_EXECUTION
  if (seCoreTermsAdd(&contextAlloc) != 0)
    COR_X(1, "the Service Execution terms could not be added to the core context");

  ldServiceDescriptionAccepted = true;
#endif

  if (ldCoreTermIdsInit(&contextAlloc) != 0)
    COR_X(1, "the core context terms could not be given their CorTerm ids");

  if (ldInit() != 0)
    COR_X(1, "ldInit failed");

  tenantInit("cor");

  if (dbStart() != 0)
    COR_X(1, "dbStart failed");

  if (troeStart() != 0)
    COR_X(1, "troeStart failed");

  //
  // The caches the create routines consult (a subscription's, a registration's), as the broker loads them
  //
  static char startupKallocBuf[16384];
  corAllocBufferInit(&corRest.kalloc, startupKallocBuf, sizeof(startupKallocBuf), 4096, NULL, "startup");
  corRest.corJsonP = corJsonCreate(&corRest.corJson, &corRest.kalloc);
  corRest.kallocP  = &corRest.kalloc;

  tenantSubCacheReload();
  tenantRegCacheReload();
  contextCacheReload();

  //
  // The current state is written as it is; a store that records history at its write sites
  // (--troe corDB) records none for it - the history is what the stream says (and, for an entity it
  // says nothing about, the created row the import writes itself)
  //
  corNgsild.troeSkip = true;

  int refused = migrateImport(file);

  troeStop();
  dbClose();

  return (refused == 0) ? 0 : 1;
}

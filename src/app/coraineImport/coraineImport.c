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
#include <stdlib.h>                                        // exit
#include <string.h>                                        // strrchr
#include <time.h>                                          // time

#include "corAlloc/corAlloc.h"                             // CorAlloc
#include "corAlloc/corAllocBufferInit.h"                   // corAllocBufferInit
#include "corLog/corLog.h"                                 // COR_X
#include "corArgs/corArgs.h"                               // corArgsInit, corArgsParse, corArgsPeek, CorArg, CORARGS_END
#include "corPlugin/corPlugin.h"                           // corPluginSetBaseDir
#include "corRest/corRestClient.h"                         // corRestClientInit
#include "corRest/CorRestState.h"                          // corRest

#include "corNgsild/CorNgsild.h"                           // corNgsild, ldDistributed, ldBrokerStartTimeSec

#include "db/DbDriver.h"                                   // db
#include "db/dbInit.h"                                     // dbStart
#include "db/dbClose.h"                                    // dbClose
#include "db/Tenant.h"                                     // tenantInit, tenantSubCacheReload, tenantRegCacheReload
#include "db/contextCache.h"                               // contextCacheReload
#include "troe/TroeDriver.h"                               // troe
#include "troe/troeInit.h"                                 // troeStart, troeStop
#include "plugin/pluginLoader.h"                           // pluginStoresLoad
#include "startup/startupCoreContext.h"                    // startupCoreContext
#include "startup/startupLog.h"                            // startupLog
#include "startup/startupArena.h"                          // startupArena

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

  //
  // The store plugins, before the options are parsed: they bring options of their own
  //
  char* dbPeek       = corArgsPeek(argC, argV, kargV, "--database");
  char* troePeek     = corArgsPeek(argC, argV, kargV, "--troe");
  bool  startupError = pluginStoresLoad((dbPeek != NULL) ? dbPeek : dbName, (troePeek != NULL) ? troePeek : troeName);

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
  startupLog(progName, traceLevels);

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

  const char* coreContextError = startupCoreContext(&contextAlloc);
  if (coreContextError != NULL)
    COR_X(1, "%s", coreContextError);

  tenantInit("cor");

  if (dbStart() != 0)
    COR_X(1, "dbStart failed");

  if (troeStart() != 0)
    COR_X(1, "troeStart failed");

  //
  // The caches the create routines consult (a subscription's, a registration's), as the broker loads them
  //
  startupArena();

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

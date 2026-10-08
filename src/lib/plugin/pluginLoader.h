#ifndef PLUGIN_PLUGINLOADER_H_
#define PLUGIN_PLUGINLOADER_H_

//
// FILE            pluginLoader.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                  // bool

#include "corArgs/corArgs.h"                          // CorArg



// -----------------------------------------------------------------------------
//
// pluginLoadDb - load a DB plugin by short name or full path
//
// Short name "mongoc" resolves to {baseDir}/db/currentState/mongoc.so
// A name containing '/' is treated as a full path.
// On failure, writes error detail to errorBuf (if not NULL).
//
extern int pluginLoadDb(const char* shortName, char* errorBuf, int errorBufSize);



// -----------------------------------------------------------------------------
//
// pluginLoadApi - load API plugins from a comma-separated list
//
// Each name resolves to {baseDir}/api/{name}.so.
// On failure, writes error detail to errorBuf (if not NULL).
//
extern int pluginLoadApi(const char* commaList, char* errorBuf, int errorBufSize);



// -----------------------------------------------------------------------------
//
// pluginLoadTroe - load a TRoE plugin by short name or full path
//
// Short name "timescale" resolves to {baseDir}/troe/temporal/timescale.so.
// "none" loads the no-op plugin (TRoE disabled).
// On failure, writes error detail to errorBuf (if not NULL).
//
extern int pluginLoadTroe(const char* shortName, char* errorBuf, int errorBufSize);



// -----------------------------------------------------------------------------
//
// pluginTroeArgUpdate - --troe's usage text: the TRoE plugins found, and corDB when its current-state
// plugin is there (--troe corDB is that plugin's own history, not a file under troe/temporal)
//
extern void pluginTroeArgUpdate(void);



// -----------------------------------------------------------------------------
//
// pluginLoadBridges - load bridge plugins from a comma-separated list
//
// Each name resolves to {baseDir}/bridge/{name}.so. Any number may be active.
// Loading only fills in each plugin's BridgeDriver - the transport is brought
// up later, by the driver's own init().
// On failure, writes error detail to errorBuf (if not NULL).
//
extern int pluginLoadBridges(const char* commaList, char* errorBuf, int errorBufSize);



// -----------------------------------------------------------------------------
//
// pluginArgsAdd - a plugin's options into the option table - an error stops the program
//
// corArgsAdd also applies their environment variables (CORAINE_DBDIR, ...): a value out of range or not
// a number there is the same error as on the command line.
//
extern void pluginArgsAdd(CorArg* argV);



// -----------------------------------------------------------------------------
//
// pluginStoresLoad - the DB and the TRoE plugin, their options added under a separator naming each
//
// Between corArgsInit and corArgsParse: the plugins bring options of their own. The broker and
// coraine-import load their stores this way. Returns true on an error (said on stderr).
//
extern bool pluginStoresLoad(const char* dbName, const char* troeName);

#endif  // PLUGIN_PLUGINLOADER_H_

#ifndef PLUGIN_PLUGINLOADER_H_
#define PLUGIN_PLUGINLOADER_H_

//
// FILE            pluginLoader.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//



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

#endif  // PLUGIN_PLUGINLOADER_H_

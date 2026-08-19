#ifndef PLUGIN_APIPLUGIN_H_
#define PLUGIN_APIPLUGIN_H_

//
// FILE            ApiPlugin.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include "kalloc/KAlloc.h"                            // KAlloc
#include "kargs/KArg.h"                               // KArg
#include "kjson/KjNode.h"                             // KjNode
#include "corRest/CorRestService.h"                     // CorRestServiceSimplified, CorRestParam



// -----------------------------------------------------------------------------
//
// API_PLUGINS_MAX - maximum number of API plugins that can be loaded
//
#define API_PLUGINS_MAX  16



// -----------------------------------------------------------------------------
//
// ApiPlugin - descriptor filled by an API plugin's register function
//
// In the corRest model, services are flat (verb included in each entry).
// A plugin fills a single services[] array and serviceCount.
//
typedef struct ApiPlugin
{
  const char*                alias;               // "admin", "test", etc.
  const char*                version;             // plugin version string
  KArg*                      args;                // plugin CLI args (NULL if none)
  CorRestParam*               params;              // plugin URL params (NULL if none)
  CorRestServiceSimplified*   services;            // flat array of services (each includes verb)
  int                        serviceCount;        // number of entries in services[]
  int                      (*init)(void);         // post-kargsParse init
  void                     (*close)(void);        // shutdown cleanup
  void                     (*versionInfo)(KAlloc* allocP, KjNode* root);
} ApiPlugin;



// -----------------------------------------------------------------------------
//
// ApiRegisterFunc - function signature for API plugin registration
//
typedef void (*ApiRegisterFunc)(ApiPlugin* pluginP);



// -----------------------------------------------------------------------------
//
// apiPlugins / apiPluginCount - global registry of loaded API plugins
//
extern ApiPlugin  apiPlugins[API_PLUGINS_MAX];
extern int        apiPluginCount;

#endif  // PLUGIN_APIPLUGIN_H_

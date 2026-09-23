//
// FILE            pluginLoader.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdio.h>                                    // snprintf
#include <string.h>                                   // memset, strcmp, strncpy, strchr, strtok_r

#include "corPlugin/corPlugin.h"                        // corPluginOpen, corPluginCloseAll, corPluginResolve, corPluginBaseDir, corPluginArgUpdate
#include "ktrace/kTrace.h"                            // KT_I

#include "db/DbDriver.h"                              // DbDriver, DbRegisterFunc, db
#include "troe/TroeDriver.h"                          // TroeDriver, TroeRegisterFunc, troe
#include "plugin/ApiPlugin.h"                         // ApiPlugin, ApiRegisterFunc, apiPlugins
#include "corBridge/BridgeDriver.h"                   // BridgeDriver, BridgeRegisterFunc, BRIDGES_MAX
#include "plugin/pluginLoader.h"                      // Own interface



// -----------------------------------------------------------------------------
//
// apiPlugins / apiPluginCount - global registry
//
ApiPlugin  apiPlugins[API_PLUGINS_MAX];
int        apiPluginCount = 0;



// -----------------------------------------------------------------------------
//
// bridges / bridgeCount - global registry
//
BridgeDriver  bridges[BRIDGES_MAX];
int           bridgeCount = 0;



// -----------------------------------------------------------------------------
//
// pluginLoadDb - load a DB plugin
//
int pluginLoadDb(const char* shortName, char* errorBuf, int errorBufSize)
{
  char path[512];

  memset(&db, 0, sizeof(DbDriver));

  corPluginResolve(corPluginBaseDir(), "db", "currentState", shortName, path, sizeof(path));

  char openErr[512];
  DbRegisterFunc registerFunc = (DbRegisterFunc) corPluginOpen(path, "dbRegister", openErr, sizeof(openErr));
  if (registerFunc == NULL)
  {
    if (errorBuf != NULL)
    {
      if (strchr(shortName, '/') == NULL)
        snprintf(errorBuf, errorBufSize, "DB plugin '%s' (%s): %s", shortName, path, openErr);
      else
        snprintf(errorBuf, errorBufSize, "DB plugin '%s': %s", shortName, openErr);
    }
    return -1;
  }

  registerFunc(&db);

  KT_I("db plugin loaded: %s", path);
  return 0;
}



// -----------------------------------------------------------------------------
//
// pluginLoadApi - load API plugins from a comma-separated list
//
int pluginLoadApi(const char* commaList, char* errorBuf, int errorBufSize)
{
  if (commaList == NULL)
    return 0;

  // Work on a copy (strtok_r modifies the string)
  char buf[1024];
  strncpy(buf, commaList, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char* saveptr = NULL;
  char* token   = strtok_r(buf, ",", &saveptr);

  while (token != NULL)
  {
    // Strip leading whitespace
    while (*token == ' ')
      token++;

    if (*token == '\0')
    {
      token = strtok_r(NULL, ",", &saveptr);
      continue;
    }

    if (apiPluginCount >= API_PLUGINS_MAX)
    {
      if (errorBuf != NULL)
        snprintf(errorBuf, errorBufSize, "too many API plugins (max %d)", API_PLUGINS_MAX);
      return -1;
    }

    char path[512];
    corPluginResolve(corPluginBaseDir(), "api", NULL, token, path, sizeof(path));

    char openErr[512];
    ApiRegisterFunc registerFunc = (ApiRegisterFunc) corPluginOpen(path, "apiRegister", openErr, sizeof(openErr));
    if (registerFunc == NULL)
    {
      if (errorBuf != NULL)
      {
        if (strchr(token, '/') == NULL)
          snprintf(errorBuf, errorBufSize, "API plugin '%s' (%s): %s", token, path, openErr);
        else
          snprintf(errorBuf, errorBufSize, "API plugin '%s': %s", token, openErr);
      }
      return -1;
    }

    ApiPlugin* pluginP = &apiPlugins[apiPluginCount];
    memset(pluginP, 0, sizeof(ApiPlugin));

    registerFunc(pluginP);
    apiPluginCount++;

    KT_I("api plugin loaded: %s (alias: %s)", path, pluginP->alias ? pluginP->alias : token);

    token = strtok_r(NULL, ",", &saveptr);
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// pluginLoadTroe - load a TRoE plugin
//
int pluginLoadTroe(const char* shortName, char* errorBuf, int errorBufSize)
{
  char path[512];

  memset(&troe, 0, sizeof(TroeDriver));

  corPluginResolve(corPluginBaseDir(), "troe", "temporal", shortName, path, sizeof(path));

  char openErr[512];
  TroeRegisterFunc registerFunc = (TroeRegisterFunc) corPluginOpen(path, "troeRegister", openErr, sizeof(openErr));
  if (registerFunc == NULL)
  {
    if (errorBuf != NULL)
    {
      if (strchr(shortName, '/') == NULL)
        snprintf(errorBuf, errorBufSize, "TRoE plugin '%s' (%s): %s", shortName, path, openErr);
      else
        snprintf(errorBuf, errorBufSize, "TRoE plugin '%s': %s", shortName, openErr);
    }
    return -1;
  }

  registerFunc(&troe);

  KT_I("troe plugin loaded: %s", path);
  return 0;
}



// -----------------------------------------------------------------------------
//
// pluginLoadBridges - load bridge plugins from a comma-separated list
//
// Bridges are the transports over which the broker speaks to something that is
// not an NGSI-LD client - a DDS topic, an MQTT broker. Like API plugins and
// unlike the DB and TRoE drivers, ANY NUMBER may be active at once: a
// deployment that bridges both DDS and MQTT loads both.
//
// This only loads the .so and lets it fill in its BridgeDriver. Bringing the
// transport up is init()'s job, and that happens later, once the broker has
// somewhere for an arriving sample to land.
//
int pluginLoadBridges(const char* commaList, char* errorBuf, int errorBufSize)
{
  if (commaList == NULL)
    return 0;

  // Work on a copy (strtok_r modifies the string)
  char buf[1024];
  strncpy(buf, commaList, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;

  char* saveptr = NULL;
  char* token   = strtok_r(buf, ",", &saveptr);

  while (token != NULL)
  {
    // Strip leading whitespace
    while (*token == ' ')
      token++;

    if (*token == 0)
    {
      token = strtok_r(NULL, ",", &saveptr);
      continue;
    }

    if (bridgeCount >= BRIDGES_MAX)
    {
      if (errorBuf != NULL)
        snprintf(errorBuf, errorBufSize, "too many bridge plugins (max %d)", BRIDGES_MAX);
      return -1;
    }

    char path[512];
    corPluginResolve(corPluginBaseDir(), "bridge", NULL, token, path, sizeof(path));

    char openErr[512];
    BridgeRegisterFunc registerFunc = (BridgeRegisterFunc) corPluginOpen(path, "bridgeRegister", openErr, sizeof(openErr));
    if (registerFunc == NULL)
    {
      //
      // A bridge named on the command line is an assertion being made NOW, so a
      // missing one is an error and the broker does not start. That is not in
      // conflict with a STORED Bridge object naming a plugin that is absent -
      // that is a record from the past, and it degrades to 'unavailable' rather
      // than vetoing a boot.
      //
      if (errorBuf != NULL)
      {
        if (strchr(token, '/') == NULL)
          snprintf(errorBuf, errorBufSize, "bridge plugin '%s' (%s): %s", token, path, openErr);
        else
          snprintf(errorBuf, errorBufSize, "bridge plugin '%s': %s", token, openErr);
      }
      return -1;
    }

    BridgeDriver* driverP = &bridges[bridgeCount];
    memset(driverP, 0, sizeof(BridgeDriver));

    //
    // ⭐ WHAT THIS BROKER SPEAKS, BEFORE THE PLUGIN WRITES A THING.
    //
    // The plugin fills in this struct, and the struct is OURS - allocated at
    // the size our header says. A plugin built against a newer contract knows
    // of slots that are not there, and without being told how much room it has
    // it would write them anyway, past the end of bridges[bridgeCount]. It
    // cannot be told by a parameter, because bridgeRegister takes one pointer
    // and nothing else, so it is told here and read back below as the PLUGIN's
    // own version. See the handshake note in BridgeDriver.h.
    //
    driverP->abiVersion = BRIDGE_ABI_VERSION;

    registerFunc(driverP);
    bridgeCount++;

    //
    // A mismatch is reported, not refused. The structs are append-only and the
    // broker owns their allocation, so an older plugin has simply left the
    // newer slots NULL - which is already how "not supported" is spelled.
    // Refusing to load would turn a working deployment red over a capability it
    // never asked for.
    //
    if (driverP->abiVersion != BRIDGE_ABI_VERSION)
      KT_I("bridge plugin '%s' was built against bridge ABI %d, this broker speaks %d - newer entry points will be treated as unsupported",
           (driverP->alias != NULL) ? driverP->alias : token, driverP->abiVersion, BRIDGE_ABI_VERSION);

    KT_I("bridge plugin loaded: %s (alias: %s)", path, (driverP->alias != NULL) ? driverP->alias : token);

    token = strtok_r(NULL, ",", &saveptr);
  }

  return 0;
}

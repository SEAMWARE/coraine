//
// FILE            pluginLoader.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stdio.h>                                    // snprintf
#include <string.h>                                   // memset, strcmp, strncpy, strchr, strtok_r

#include "swPlugin/swPlugin.h"                        // swPluginOpen, swPluginCloseAll, swPluginResolve, swPluginBaseDir, swPluginArgUpdate
#include "ktrace/kTrace.h"                            // KT_I

#include "db/DbDriver.h"                              // DbDriver, DbRegisterFunc, db
#include "troe/TroeDriver.h"                          // TroeDriver, TroeRegisterFunc, troe
#include "plugin/ApiPlugin.h"                         // ApiPlugin, ApiRegisterFunc, apiPlugins
#include "plugin/pluginLoader.h"                      // Own interface



// -----------------------------------------------------------------------------
//
// apiPlugins / apiPluginCount - global registry
//
ApiPlugin  apiPlugins[API_PLUGINS_MAX];
int        apiPluginCount = 0;



// -----------------------------------------------------------------------------
//
// pluginLoadDb - load a DB plugin
//
int pluginLoadDb(const char* shortName, char* errorBuf, int errorBufSize)
{
  char path[512];

  memset(&db, 0, sizeof(DbDriver));

  swPluginResolve(swPluginBaseDir(), "db", "currentState", shortName, path, sizeof(path));

  char openErr[512];
  DbRegisterFunc registerFunc = (DbRegisterFunc) swPluginOpen(path, "dbRegister", openErr, sizeof(openErr));
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
    swPluginResolve(swPluginBaseDir(), "api", NULL, token, path, sizeof(path));

    char openErr[512];
    ApiRegisterFunc registerFunc = (ApiRegisterFunc) swPluginOpen(path, "apiRegister", openErr, sizeof(openErr));
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

  swPluginResolve(swPluginBaseDir(), "troe", "temporal", shortName, path, sizeof(path));

  char openErr[512];
  TroeRegisterFunc registerFunc = (TroeRegisterFunc) swPluginOpen(path, "troeRegister", openErr, sizeof(openErr));
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

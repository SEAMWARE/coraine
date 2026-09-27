//
// FILE            adminLog.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                               // NULL
#include <string.h>                               // strcmp

#include "corAlloc/CorAlloc.h"                    // CorAlloc
#include "corTree/corTreeBuilder.h"               // corTreeObject, corTreeString, corTreeBoolean, corTreeChildAdd
#include "corRest/CorRestState.h"                   // corRest
#include "corLog/corLogGlobals.h"                 // corLogVerbose, corLogDebug, corLogInfo
#include "corLog/corLogTraceLevelGet.h"           // corLogTraceLevelGet
#include "corLog/corLogTraceLevelSet.h"           // corLogTraceLevelSet
#include "corLog/corLogTraceLevelReset.h"         // corLogTraceLevelReset

#include "api/admin/adminLog.h"                   // Own interface



// -----------------------------------------------------------------------------
//
// kvLookup - look up a value by key in a CorRestKeyValue array
//
static const char* kvLookup(CorRestKeyValue* kvV, int kvCount, const char* key)
{
  for (int i = 0; i < kvCount; i++)
  {
    if (kvV[i].key != NULL && strcmp(kvV[i].key, key) == 0)
      return kvV[i].value;
  }
  return NULL;
}



// -----------------------------------------------------------------------------
//
// boolFromOnOff - parse "on"/"off"/"true"/"false"/"1"/"0" to 0/1, or -1 on error
//
static int boolFromOnOff(const char* s)
{
  if (s == NULL) return -1;
  if (strcmp(s, "on")    == 0 || strcmp(s, "true")  == 0 || strcmp(s, "1") == 0)  return 1;
  if (strcmp(s, "off")   == 0 || strcmp(s, "false") == 0 || strcmp(s, "0") == 0)  return 0;
  return -1;
}



// -----------------------------------------------------------------------------
//
// adminGetLog - GET /admin/log
//
bool adminGetLog(void)
{
  CorAlloc*   allocP = corRest.kallocP;
  CorNode*    root   = corTreeObject(allocP, NULL);
  const char* levels = corLogTraceLevelGet();

  corTreeChildAdd(root, corTreeBoolean(allocP, "verbose", corLogVerbose));
  corTreeChildAdd(root, corTreeBoolean(allocP, "debug", corLogDebug));
  corTreeChildAdd(root, corTreeBoolean(allocP, "info", corLogInfo));
  corTreeChildAdd(root, corTreeString(allocP, "traceLevels", levels ? levels : ""));

  corRest.out.responseTree = root;
  return true;
}



// -----------------------------------------------------------------------------
//
// adminLogApplyFlags - apply verbose/debug/info params if present
//
static void adminLogApplyFlags(void)
{
  const char* v;

  v = kvLookup(corRest.in.uriParamV, corRest.in.uriParamCount, "verbose");
  if (v != NULL)
  {
    int b = boolFromOnOff(v);
    if (b >= 0)
      corLogVerbose = (b == 1);
  }

  v = kvLookup(corRest.in.uriParamV, corRest.in.uriParamCount, "debug");
  if (v != NULL)
  {
    int b = boolFromOnOff(v);
    if (b >= 0)
      corLogDebug = (b == 1);
  }

  v = kvLookup(corRest.in.uriParamV, corRest.in.uriParamCount, "info");
  if (v != NULL)
  {
    int b = boolFromOnOff(v);
    if (b >= 0)
      corLogInfo = (b == 1);
  }
}



// -----------------------------------------------------------------------------
//
// adminPutLog - PUT /admin/log
//
bool adminPutLog(void)
{
  adminLogApplyFlags();

  const char* levels = kvLookup(corRest.in.uriParamV, corRest.in.uriParamCount, "traceLevels");
  if (levels != NULL)
    corLogTraceLevelSet(levels, true);    // replace=true

  return adminGetLog();
}



// -----------------------------------------------------------------------------
//
// adminPostLog - POST /admin/log
//
bool adminPostLog(void)
{
  adminLogApplyFlags();

  const char* levels = kvLookup(corRest.in.uriParamV, corRest.in.uriParamCount, "traceLevels");
  if (levels != NULL)
    corLogTraceLevelSet(levels, false);   // replace=false (additive)

  return adminGetLog();
}



// -----------------------------------------------------------------------------
//
// adminPatchLog - PATCH /admin/log (alias for POST)
//
bool adminPatchLog(void)
{
  return adminPostLog();
}



// -----------------------------------------------------------------------------
//
// adminDeleteLog - DELETE /admin/log
//
bool adminDeleteLog(void)
{
  adminLogApplyFlags();

  const char* levels = kvLookup(corRest.in.uriParamV, corRest.in.uriParamCount, "traceLevels");
  if (levels != NULL)
    corLogTraceLevelReset(levels);

  return adminGetLog();
}

//
// FILE            adminLog.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stddef.h>                               // NULL
#include <string.h>                               // strcmp

#include "kjson/kjBuilder.h"                      // kjObject, kjString, kjBoolean, kjChildAdd
#include "swRest/SwRestState.h"                   // swRest
#include "ktrace/ktGlobals.h"                     // ktVerbose, ktDebug, ktInfo
#include "ktrace/ktTraceLevelGet.h"               // ktTraceLevelGet
#include "ktrace/ktTraceLevelSet.h"               // ktTraceLevelSet
#include "ktrace/ktTraceLevelReset.h"             // ktTraceLevelReset

#include "api/admin/adminLog.h"                   // Own interface



// -----------------------------------------------------------------------------
//
// kvLookup - look up a value by key in a SwRestKeyValue array
//
static const char* kvLookup(SwRestKeyValue* kvV, int kvCount, const char* key)
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
  Kjson*      kjsonP = swRest.kjsonP;
  KjNode*     root   = kjObject(kjsonP, NULL);
  const char* levels = ktTraceLevelGet();

  kjChildAdd(root, kjBoolean(kjsonP, "verbose", ktVerbose));
  kjChildAdd(root, kjBoolean(kjsonP, "debug",   ktDebug));
  kjChildAdd(root, kjBoolean(kjsonP, "info",    ktInfo));
  kjChildAdd(root, kjString(kjsonP,  "traceLevels", levels ? levels : ""));

  swRest.out.responseTree = root;
  return true;
}



// -----------------------------------------------------------------------------
//
// adminLogApplyFlags - apply verbose/debug/info params if present
//
static void adminLogApplyFlags(void)
{
  const char* v;

  v = kvLookup(swRest.in.uriParamV, swRest.in.uriParamCount, "verbose");
  if (v != NULL)
  {
    int b = boolFromOnOff(v);
    if (b >= 0)
      ktVerbose = (b == 1);
  }

  v = kvLookup(swRest.in.uriParamV, swRest.in.uriParamCount, "debug");
  if (v != NULL)
  {
    int b = boolFromOnOff(v);
    if (b >= 0)
      ktDebug = (b == 1);
  }

  v = kvLookup(swRest.in.uriParamV, swRest.in.uriParamCount, "info");
  if (v != NULL)
  {
    int b = boolFromOnOff(v);
    if (b >= 0)
      ktInfo = (b == 1);
  }
}



// -----------------------------------------------------------------------------
//
// adminPutLog - PUT /admin/log
//
bool adminPutLog(void)
{
  adminLogApplyFlags();

  const char* levels = kvLookup(swRest.in.uriParamV, swRest.in.uriParamCount, "traceLevels");
  if (levels != NULL)
    ktTraceLevelSet(levels, KTRUE);   // replace=true

  return adminGetLog();
}



// -----------------------------------------------------------------------------
//
// adminPostLog - POST /admin/log
//
bool adminPostLog(void)
{
  adminLogApplyFlags();

  const char* levels = kvLookup(swRest.in.uriParamV, swRest.in.uriParamCount, "traceLevels");
  if (levels != NULL)
    ktTraceLevelSet(levels, KFALSE);  // replace=false (additive)

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

  const char* levels = kvLookup(swRest.in.uriParamV, swRest.in.uriParamCount, "traceLevels");
  if (levels != NULL)
    ktTraceLevelReset(levels);

  return adminGetLog();
}

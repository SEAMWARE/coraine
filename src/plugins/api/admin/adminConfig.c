//
// FILE            adminConfig.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                              // bool
#include <string.h>                               // strcmp

#include "corArgs/CorArgInfo.h"                   // CorArgInfo, CorArgValueFrom
#include "corArgs/corArgsInit.h"                  // corArgInfoV
#include "corRest/CorRestState.h"                 // corRest
#include "corNgsild/corNgsild.h"                    // corNgsild
#include "corTree/CorNode.h"                      // CorNode
#include "corTree/corTreeBuilder.h"               // corTreeObject, corTreeArray, corTreeString, corTreeChildAdd, ...

#include "api/admin/adminConfig.h"                // Own interface



// -----------------------------------------------------------------------------
//
// typeName - the option's type as GET /admin/config names it
//
static const char* typeName(CorArgType type)
{
  switch (type)
  {
  case CorArgBool:    return "bool";
  case CorArgString:  return "string";
  case CorArgFloat:   return "float";
  case CorArgInt8:
  case CorArgUInt8:
  case CorArgInt16:
  case CorArgUInt16:
  case CorArgInt32:
  case CorArgUInt32:
  case CorArgInt64:
  case CorArgUInt64:  return "int";
  default:            return "unknown";
  }
}



// -----------------------------------------------------------------------------
//
// fromName - where the option's value came from
//
static const char* fromName(CorArgValueFrom from)
{
  switch (from)
  {
  case CorArgFromDefaultValue:  return "default";
  case CorArgFromConfigFile:    return "configFile";
  case CorArgFromEnvVar:        return "env";
  case CorArgFromShortOption:   return "shortOption";
  case CorArgFromLongOption:    return "longOption";
  }

  return "unknown";
}



// -----------------------------------------------------------------------------
//
// valueNode - a value as JSON: the one in valueP (isDefault false) or the default - stored in 'def' as corArgs
// keeps it, a number or a bool cast to the pointer, a string the pointer itself. A secret that is set: "***"
//
static CorNode* valueNode(CorArgInfo* aP, const char* name, bool isDefault)
{
  CorAlloc* allocP = corRest.kallocP;
  void*     vP     = aP->valueP;
  void*     def    = aP->def;

  if (aP->type == CorArgString)
  {
    const char* s = isDefault ? (const char*) def : *((char**) vP);

    if (s == NULL)
      return corTreeNull(allocP, name);

    return corTreeString(allocP, name, aP->secret ? "***" : s);
  }

  if (aP->secret)
    return corTreeString(allocP, name, "***");

  switch (aP->type)
  {
  case CorArgBool:    return corTreeBoolean(allocP, name, isDefault ? ((long long) def != 0) : *((bool*) vP));
  case CorArgFloat:   return corTreeFloat(allocP, name, isDefault ? (double) (long long) def : (double) *((float*) vP));
  case CorArgInt8:    return corTreeInteger(allocP, name, isDefault ? (char) (long long) def : *((char*) vP));
  case CorArgUInt8:   return corTreeInteger(allocP, name, isDefault ? (unsigned char) (long long) def : *((unsigned char*) vP));
  case CorArgInt16:   return corTreeInteger(allocP, name, isDefault ? (short) (long long) def : *((short*) vP));
  case CorArgUInt16:  return corTreeInteger(allocP, name, isDefault ? (unsigned short) (long long) def : *((unsigned short*) vP));
  case CorArgInt32:   return corTreeInteger(allocP, name, isDefault ? (int) (long long) def : *((int*) vP));
  case CorArgUInt32:  return corTreeInteger(allocP, name, isDefault ? (unsigned int) (long long) def : *((unsigned int*) vP));
  case CorArgInt64:   return corTreeInteger(allocP, name, isDefault ? (long long) def : *((long long*) vP));
  case CorArgUInt64:  return corTreeInteger(allocP, name, (long long) (isDefault ? (unsigned long long) def : *((unsigned long long*) vP)));
  default:            return corTreeNull(allocP, name);
  }
}



// -----------------------------------------------------------------------------
//
// adminGetConfig -
//
bool adminGetConfig(void)
{
  CorAlloc*   allocP  = corRest.kallocP;
  CorNode*    root    = corTreeObject(allocP, NULL);
  CorNode*    optsP   = corTreeArray(allocP, "options");
  const char* section = NULL;

  for (int ix = 0; (corArgInfoV != NULL) && (corArgInfoV[ix].type != CorArgEnd); ix++)
  {
    CorArgInfo* aP = &corArgInfoV[ix];

    if (aP->type == CorArgSeparator)                 // a plugin's options start: "Database (mongoc) plugin options"
    {
      section = aP->description;
      continue;
    }

    if ((aP->valueP == NULL) || (aP->disabled == true))
      continue;

    if ((aP->longName != NULL) && ((strcmp(aP->longName, "--usage") == 0) || (strcmp(aP->longName, "--Usage") == 0)))
      continue;

    CorNode* oP = corTreeObject(allocP, NULL);

    corTreeChildAdd(oP, corTreeString(allocP, "name", (aP->longName != NULL) ? aP->longName : aP->shortName));
    if ((aP->shortName != NULL) && (aP->longName != NULL))
      corTreeChildAdd(oP, corTreeString(allocP, "short", aP->shortName));
    corTreeChildAdd(oP, corTreeString(allocP, "type", typeName(aP->type)));
    corTreeChildAdd(oP, valueNode(aP, "value", false));
    corTreeChildAdd(oP, valueNode(aP, "default", true));
    corTreeChildAdd(oP, corTreeString(allocP, "from", fromName(aP->from)));
    if (aP->envVar[0] != 0)
      corTreeChildAdd(oP, corTreeString(allocP, "env", aP->envVar));
    if (aP->secret)
      corTreeChildAdd(oP, corTreeBoolean(allocP, "secret", true));
    if (section != NULL)
      corTreeChildAdd(oP, corTreeString(allocP, "section", section));
    if (aP->description != NULL)
      corTreeChildAdd(oP, corTreeString(allocP, "description", aP->description));

    corTreeChildAdd(optsP, oP);
  }

  corTreeChildAdd(root, optsP);
  corRest.out.responseTree = root;
  corNgsild.rawResponse = true;                       // not an Entity

  return true;
}

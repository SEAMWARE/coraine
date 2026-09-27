//
// FILE            adminPlugins.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                               // NULL

#include "kalloc/KAlloc.h"                        // KAlloc
#include "corTree/corTreeBuilder.h"               // corTreeObject, corTreeArray, corTreeString, corTreeChildAdd
#include "corRest/CorRestState.h"                   // corRest

#include "db/DbDriver.h"                         // db
#include "plugin/ApiPlugin.h"                     // apiPlugins, apiPluginCount

#include "api/admin/adminPlugins.h"               // Own interface



// -----------------------------------------------------------------------------
//
// adminGetPlugins -
//
// Returns a JSON object with loaded plugin info:
// {
//   "db": "mongoc",
//   "api": [ "admin" ]
// }
//
bool adminGetPlugins(void)
{
  KAlloc* allocP = corRest.kallocP;
  CorNode* root  = corTreeObject(allocP, NULL);

  // DB plugin
  corTreeChildAdd(root, corTreeString(allocP, "db", db.alias ? db.alias : "none"));

  // API plugins
  CorNode* apiArray = corTreeArray(allocP, "api");
  for (int i = 0; i < apiPluginCount; i++)
    corTreeChildAdd(apiArray, corTreeString(allocP, NULL, apiPlugins[i].alias ? apiPlugins[i].alias : "?"));
  corTreeChildAdd(root, apiArray);

  corRest.out.responseTree = root;
  return true;
}

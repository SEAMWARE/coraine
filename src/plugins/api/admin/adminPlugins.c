//
// FILE            adminPlugins.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stddef.h>                               // NULL

#include "kjson/kjBuilder.h"                      // kjObject, kjArray, kjString, kjChildAdd
#include "swRest/SwRestState.h"                   // swRest

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
  Kjson*  kjsonP = swRest.kjsonP;
  KjNode* root   = kjObject(kjsonP, NULL);

  // DB plugin
  kjChildAdd(root, kjString(kjsonP, "db", db.alias ? db.alias : "none"));

  // API plugins
  KjNode* apiArray = kjArray(kjsonP, "api");
  for (int i = 0; i < apiPluginCount; i++)
    kjChildAdd(apiArray, kjString(kjsonP, NULL, apiPlugins[i].alias ? apiPlugins[i].alias : "?"));
  kjChildAdd(root, apiArray);

  swRest.out.responseTree = root;
  return true;
}

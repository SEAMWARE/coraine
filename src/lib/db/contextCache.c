//
// FILE            contextCache.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stddef.h>                                   // NULL

#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAllocStrdup.h"                  // corAllocStrdup
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corLog/corLog.h"                            // KT_*

#include "corJsonld/CorLdContext.h"                     // CorLdContext
#include "corJsonld/CorLdContextCache.h"                // CorLdContextCache
#include "corJsonld/corLdCache.h"                       // corLdCacheInsert, corLdCacheRemove
#include "corJsonld/corLdContextParse.h"                // corLdContextFromObject

#include "db/DbDriver.h"                              // db, DbContextRow
#include "db/contextCache.h"                          // Own interface



// -----------------------------------------------------------------------------
//
// corLdCacheGet - internal accessor in corJsonld/corLdInit.c (cache allocator)
//
extern CorLdContextCache* corLdCacheGet(void);



// -----------------------------------------------------------------------------
//
// contextRowToCache - turn one persisted row into a cached @context
//
// The one place a stored row becomes a cache item, so the startup load and the
// HA apply cannot end up with two different ideas of what a stored @context is.
//
static bool contextRowToCache(DbContextRow* rowP)
{
  if ((rowP->id == NULL) || (rowP->body == NULL))
    return false;

  CorAlloc* storeP = corLdCacheGet()->kaP;

  //
  // Parse the body (a stand-alone JSON-LD context document) into a tree and pull
  // out @context. The cache allocator is used so the result outlives this call.
  //
  char*  bodyForParse = corAllocStrdup(storeP, rowP->body);  // corJsonParse is destructive
  CorJson corJson;
  CorJson* corJsonP = corJsonCreate(&corJson, storeP);

  CorNode* treeP = corJsonParse(corJsonP, bodyForParse);

  if (treeP == NULL)
    return false;

  CorNode* atContextP = corTreeLookup(treeP, "@context");

  if ((atContextP == NULL) || (atContextP->type != CorObject))
    return false;

  CorLdContext* contextP = corLdContextFromObject(atContextP, storeP, rowP->url);

  if (contextP == NULL)
    return false;

  contextP->id   = corAllocStrdup(storeP, rowP->id);
  contextP->body = corAllocStrdup(storeP, rowP->body);
  contextP->kind = (rowP->kind == DB_CONTEXT_KIND_HOSTED)? CorLdKindHosted : CorLdKindCached;

  corLdCacheInsert(contextP);

  return true;
}



// -----------------------------------------------------------------------------
//
// contextCacheReload -
//
void contextCacheReload(void)
{
  if (db.contextList == NULL)
    return;

  DbContextRow* rows  = NULL;
  int           count = 0;

  if (db.contextList(corLdCacheGet()->kaP, &rows, &count) != DB_OK)
    return;

  for (int ix = 0; ix < count; ix++)
    contextRowToCache(&rows[ix]);
}



// -----------------------------------------------------------------------------
//
// contextCacheItemRefresh -
//
bool contextCacheItemRefresh(const char* id)
{
  if (db.contextGet == NULL)
    return false;

  DbContextRow row = { NULL, NULL, 0, NULL };
  int          r   = db.contextGet(id, corLdCacheGet()->kaP, &row);

  if (r == DB_NOT_FOUND)
  {
    contextCacheItemDrop(id);
    return true;
  }

  if (r != DB_OK)
    return false;

  //
  // The cache is a list, not a map - an insert on top of a cached id would leave
  // both copies in it, and which one a lookup finds is then a matter of list
  // order.
  //
  corLdCacheRemove(id);

  return contextRowToCache(&row);
}



// -----------------------------------------------------------------------------
//
// contextCacheItemDrop -
//
bool contextCacheItemDrop(const char* id)
{
  return (corLdCacheRemove(id) != NULL);
}

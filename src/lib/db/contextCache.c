//
// FILE            contextCache.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                   // NULL

#include "kalloc/KAlloc.h"                            // KAlloc
#include "kalloc/kaStrdup.h"                          // kaStrdup
#include "kjson/kjson.h"                              // Kjson
#include "kjson/kjBufferCreate.h"                     // kjBufferCreate
#include "kjson/kjParse.h"                            // kjParse
#include "kjson/kjLookup.h"                           // kjLookup
#include "ktrace/kTrace.h"                            // KT_*

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

  KAlloc* storeP = corLdCacheGet()->kaP;

  //
  // Parse the body (a stand-alone JSON-LD context document) into a tree and pull
  // out @context. The cache allocator is used so the result outlives this call.
  //
  char*  bodyForParse = kaStrdup(storeP, rowP->body);  // kjParse is destructive
  Kjson  kjson;
  Kjson* kjsonP = kjBufferCreate(&kjson, storeP);

  KjNode* treeP = kjParse(kjsonP, bodyForParse);

  if (treeP == NULL)
    return false;

  KjNode* atContextP = kjLookup(treeP, "@context");

  if ((atContextP == NULL) || (atContextP->type != KjObject))
    return false;

  CorLdContext* contextP = corLdContextFromObject(atContextP, storeP, rowP->url);

  if (contextP == NULL)
    return false;

  contextP->id   = kaStrdup(storeP, rowP->id);
  contextP->body = kaStrdup(storeP, rowP->body);
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

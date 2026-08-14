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

#include "swJsonld/SwldContext.h"                     // SwldContext
#include "swJsonld/SwldContextCache.h"                // SwldContextCache
#include "swJsonld/swldCache.h"                       // swldCacheInsert, swldCacheRemove
#include "swJsonld/swldContextParse.h"                // swldContextFromObject

#include "db/DbDriver.h"                              // db, DbContextRow
#include "db/contextCache.h"                          // Own interface



// -----------------------------------------------------------------------------
//
// swldCacheGet - internal accessor in swJsonld/swldInit.c (cache allocator)
//
extern SwldContextCache* swldCacheGet(void);



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

  KAlloc* storeP = swldCacheGet()->kaP;

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

  SwldContext* contextP = swldContextFromObject(atContextP, storeP, rowP->url);

  if (contextP == NULL)
    return false;

  contextP->id   = kaStrdup(storeP, rowP->id);
  contextP->body = kaStrdup(storeP, rowP->body);
  contextP->kind = (rowP->kind == DB_CONTEXT_KIND_HOSTED)? SwldKindHosted : SwldKindCached;

  swldCacheInsert(contextP);

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

  if (db.contextList(swldCacheGet()->kaP, &rows, &count) != DB_OK)
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
  int          r   = db.contextGet(id, swldCacheGet()->kaP, &row);

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
  swldCacheRemove(id);

  return contextRowToCache(&row);
}



// -----------------------------------------------------------------------------
//
// contextCacheItemDrop -
//
bool contextCacheItemDrop(const char* id)
{
  return (swldCacheRemove(id) != NULL);
}

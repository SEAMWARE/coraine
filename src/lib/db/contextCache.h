#ifndef DB_CONTEXTCACHE_H_
#define DB_CONTEXTCACHE_H_

//
// FILE            contextCache.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// The @context cache, loaded from the persisted context rows.
//
// Unlike the subscription and registration caches this one is NOT per tenant:
// an @context is identified by its URL, and the document at a URL is the same
// whoever fetched it. The rows live in the reserved global database, not in a
// tenant's.
//
#include <stdbool.h>                                     // bool



// -----------------------------------------------------------------------------
//
// contextCacheReload - re-populate the @context cache from the persisted rows
//
// Called once at startup, after dbStart().
//
extern void contextCacheReload(void);



// -----------------------------------------------------------------------------
//
// contextCacheItemRefresh - re-read one @context row into the cache
//
// For a caller told that the row changed but not what it now says - the HA
// apply. Reads the row, never the network: what the URL would serve is not
// necessarily what the instance that persisted this row saw.
//
// A row that is gone is not an error - the cached copy is dropped instead.
//
extern bool contextCacheItemRefresh(const char* id);



// -----------------------------------------------------------------------------
//
// contextCacheItemDrop - remove one @context from the cache (cache only)
//
extern bool contextCacheItemDrop(const char* id);

#endif  // DB_CONTEXTCACHE_H_

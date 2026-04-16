//
// FILE            getJsonldContext.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/jsonldContexts/{contextId} — Retrieve JSON-LD Context.
// NGSI-LD v1.9.1 § 5.13.4.
//
// {contextId} is the context URL (URL-encoded in the path; the REST layer
// decodes it into swRest.in.wildcard[0]). Response is the raw JSON-LD body
// as received at download time.
//
// Lazy-reload: if the entry isn't in the in-memory cache (LRU eviction
// under churn), fall back to the persisted "swBroker" DB and reinstate it
// before responding. Lets persisted Hosted/Cached survive eviction.
//

#include <stddef.h>                                    // NULL
#include <string.h>                                    // strlen

#include "swRest/SwRestState.h"                        // swRest
#include "kjson/kjson.h"                               // Kjson
#include "kjson/kjBufferCreate.h"                      // kjBufferCreate
#include "kjson/kjParse.h"                             // kjParse
#include "kjson/kjLookup.h"                            // kjLookup
#include "kalloc/kaStrdup.h"                           // kaStrdup
#include "swJsonld/SwldContext.h"                      // SwldContext, SwldContextKind
#include "swJsonld/SwldContextCache.h"                 // SwldContextCache
#include "swJsonld/swldCache.h"                        // swldCacheLookup, swldCacheInsert
#include "swJsonld/swldContextParse.h"                 // swldContextFromObject
#include "swNgsild/swNgsild.h"                         // ldError, LD_ERROR_*, swNgsild

#include "db/DbDriver.h"                               // db, DB_OK, DB_CONTEXT_KIND_*

#include "serviceRoutines/getJsonldContext.h"          // Own interface



// -----------------------------------------------------------------------------
//
// swldCacheGet - internal accessor in swJsonld/swldInit.c (cache allocator)
//
extern SwldContextCache* swldCacheGet(void);



// -----------------------------------------------------------------------------
//
// loadFromDb - rehydrate one persisted context into the cache and return it.
//
// Returns NULL if the row isn't in the DB or if reconstruction fails.
//
static SwldContext* loadFromDb(const char* contextId)
{
  if (db.contextGet == NULL)
    return NULL;

  KAlloc* storeP = swldCacheGet()->kaP;

  DbContextRow row;
  if (db.contextGet(contextId, storeP, &row) != DB_OK)
    return NULL;

  if (row.body == NULL)
    return NULL;

  //
  // kjParse is destructive — keep a pristine copy for contextP->body
  // before handing the original to the parser.
  //
  char*  bodyCopy = kaStrdup(storeP, row.body);

  Kjson  kjson;
  Kjson* kjsonP = kjBufferCreate(&kjson, storeP);

  KjNode* treeP = kjParse(kjsonP, row.body);
  if (treeP == NULL)
    return NULL;

  KjNode* atContextP = kjLookup(treeP, "@context");
  if (atContextP == NULL || atContextP->type != KjObject)
    return NULL;

  SwldContext* contextP = swldContextFromObject(atContextP, storeP, row.url);
  if (contextP == NULL)
    return NULL;

  contextP->id   = (row.id != NULL) ? row.id : kaStrdup(storeP, contextId);
  contextP->body = bodyCopy;
  contextP->kind = (row.kind == DB_CONTEXT_KIND_HOSTED) ? SwldKindHosted : SwldKindCached;

  swldCacheInsert(contextP);

  // Look it up again so we get the cache-resident pointer, in case insert
  // dropped it as a duplicate (shouldn't, but cheap to be sure).
  SwldContext* live = swldCacheLookup(contextId);
  return (live != NULL) ? live : contextP;
}



// -----------------------------------------------------------------------------
//
// getJsonldContext -
//
bool getJsonldContext(void)
{
  const char* contextId = swRest.in.wildcard[0];

  if (contextId == NULL || contextId[0] == '\0')
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "context id is missing");
    return true;
  }

  SwldContext* contextP = swldCacheLookup(contextId);

  if (contextP == NULL)
  {
    // Lazy-reload from persistent storage (Hosted/Cached only — Implicit
    // is never persisted).
    contextP = loadFromDb(contextId);
  }

  if (contextP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "JSON-LD context '%s' not found", contextId);
    return true;
  }

  if (contextP->body == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "JSON-LD context '%s' body not available", contextId);
    return true;
  }

  swRest.out.payload     = contextP->body;
  swRest.out.payloadSize = strlen(contextP->body);
  swRest.out.contentType = (char*) "application/ld+json";

  swRest.out.httpStatusCode = 200;
  return true;
}

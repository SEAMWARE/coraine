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
#include <stdio.h>                                     // snprintf
#include <string.h>                                    // strlen, memcpy
#include <time.h>                                      // gmtime_r, struct tm

#include "swRest/SwRestState.h"                        // swRest
#include "swRest/swRestOutHeader.h"                    // swRestOutHeaderAdd
#include "kjson/kjson.h"                               // Kjson
#include "kjson/kjBuilder.h"                           // kjObject, kjString, kjInteger, kjChildAdd
#include "kjson/kjBufferCreate.h"                      // kjBufferCreate
#include "kjson/kjParse.h"                             // kjParse
#include "kjson/kjLookup.h"                            // kjLookup
#include "kalloc/kaAlloc.h"                            // kaAlloc
#include "kalloc/kaStrdup.h"                           // kaStrdup
#include "swJsonld/SwldContext.h"                      // SwldContext, SwldContextKind
#include "swJsonld/SwldContextCache.h"                 // SwldContextCache
#include "swJsonld/swldCache.h"                        // swldCacheLookup, swldCacheInsert
#include "swJsonld/swldContextParse.h"                 // swldContextFromObject, swldContextFromTree
#include "swJsonld/swldUrlResolve.h"                   // swldUrlResolve
#include "swJsonld/swldDownload.h"                     // swldContextFromUrl, swldIsCoreContextUrl
#include "swJsonld/swldInit.h"                         // swldCoreContext
#include "swNgsild/swNgsild.h"                         // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/SwNgsild.h"                         // ldBrokerHttpEndpoint

#include "db/DbDriver.h"                               // db, DB_OK, DB_CONTEXT_KIND_*

#include "serviceRoutines/getJsonldContext.h"          // Own interface



// -----------------------------------------------------------------------------
//
// kindString - NGSI-LD § 5.13.1 kind name
//
static const char* kindString(SwldContextKind k)
{
  switch (k)
  {
    case SwldKindHosted:   return "Hosted";
    case SwldKindCached:   return "Cached";
    case SwldKindImplicit: return "ImplicitlyCreated";
  }
  return "ImplicitlyCreated";
}



// -----------------------------------------------------------------------------
//
// epochToIso - render a unix-epoch (seconds, with optional fractional ms)
// as an ISO 8601 DateTime string in UTC. Buffer lives in swRest.kalloc.
// § 5.13.3.5 createdAt / lastUsage are DateTime strings (parseable by
// ETSI's `Parse Ngsild Date`), not Unix timestamps.
//
static char* epochToIso(double t)
{
  time_t   secs = (time_t) t;
  struct tm tm;
  gmtime_r(&secs, &tm);
  char* buf = (char*) kaAlloc(&swRest.kalloc, 80);
  snprintf(buf, 80, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec);
  return buf;
}



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
  if (atContextP == NULL)
    return NULL;

  // Object → single map; Array → wrapper context; String → indirect URL
  // (the persisted body says "this @context lives at <url>"). Same code
  // path as postJsonldContexts; covers Hosted, Cached, and the
  // ImplicitlyCreated entries auto-generated for Subscription bodies.
  SwldContext* contextP = NULL;
  if (atContextP->type == KjObject)
    contextP = swldContextFromObject(atContextP, storeP, row.url);
  else if (atContextP->type == KjArray)
    // row.url is the base any RELATIVE reference inside resolves against - it is
    // set for a Cached @context, and NULL for a Hosted one, which has no URL
    contextP = swldContextFromTree(atContextP, storeP, row.url);
  else if (atContextP->type == KjString)
    contextP = swldContextFromUrl(swldUrlResolve(row.url, atContextP->value.s, storeP), storeP);
  if (contextP == NULL)
    return NULL;

  contextP->id   = (row.id != NULL) ? row.id : kaStrdup(storeP, contextId);
  contextP->body = bodyCopy;
  contextP->kind = (row.kind == DB_CONTEXT_KIND_HOSTED)   ? SwldKindHosted
                  : (row.kind == DB_CONTEXT_KIND_IMPLICIT) ? SwldKindImplicit
                  :                                         SwldKindCached;

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
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Missing URL Component", "context id is missing");
    return true;
  }

  //
  // Any recognised core URL (configured / unversioned / older version)
  // refers to THE core entry — the older versions are ignored stubs for
  // expansion, but the admin API serves the one core (ETSI 051_09 GETs
  // the core's metadata via an older-version URL).
  //
  if (swldIsCoreContextUrl(contextId))
  {
    SwldContext* coreP = swldCoreContext();
    if (coreP != NULL && coreP->url != NULL)
      contextId = coreP->url;
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

  // § 5.13.4.4 / § 5.13.3.5: ?details=true returns metadata about the
  // @context (URL, localId, kind, createdAt, ...) instead of the body
  // itself.
  //
  // URL field semantics:
  //   - Cached:  the URL the broker downloaded from (stored as contextP->url).
  //   - Hosted / ImplicitlyCreated: the URL where the broker serves the
  //     context. Built from --httpEndpoint when set, otherwise as a
  //     server-relative path (the test fixtures only require a non-empty
  //     string).
  if (swNgsild.details)
  {
    const char* localId = (contextP->id != NULL) ? contextP->id : contextId;
    const char* urlOut  = contextP->url;
    if (urlOut == NULL)
    {
      const char* prefix = "/ngsi-ld/v1/jsonldContexts/";
      const char* base   = (ldBrokerHttpEndpoint != NULL) ? ldBrokerHttpEndpoint : "";
      int   baseLen      = strlen(base);
      int   prefixLen    = strlen(prefix);
      int   idLen        = strlen(localId);
      char* buf          = (char*) kaAlloc(&swRest.kalloc, baseLen + prefixLen + idLen + 1);
      memcpy(buf, base, baseLen);
      memcpy(buf + baseLen, prefix, prefixLen);
      memcpy(buf + baseLen + prefixLen, localId, idLen);
      buf[baseLen + prefixLen + idLen] = 0;
      urlOut = buf;
    }

    KjNode* meta = kjObject(swRest.kjsonP, NULL);
    kjChildAdd(meta, kjString(swRest.kjsonP, "URL",       (char*) urlOut));
    kjChildAdd(meta, kjString(swRest.kjsonP, "localId",   (char*) localId));
    kjChildAdd(meta, kjString(swRest.kjsonP, "kind",      (char*) kindString(contextP->kind)));
    kjChildAdd(meta, kjString(swRest.kjsonP, "createdAt", epochToIso(contextP->createdAt)));
    kjChildAdd(meta, kjString(swRest.kjsonP, "lastUsage", epochToIso(contextP->usedAt)));

    // Bypass the JSON-LD render hook — its ldStripSysAttrs would otherwise
    // remove the createdAt / modifiedAt members we just put in. The
    // jsonldContext metadata response is not an Entity / NGSI-LD document.
    swNgsild.rawResponse      = true;
    swRest.out.responseTree   = meta;
    swRest.out.httpStatusCode = 200;
    return true;
  }

  // § 5.13.4.4: details=false (or absent) — return the @context body for
  // Hosted / ImplicitlyCreated; OperationNotSupported for Cached.
  if (contextP->kind == SwldKindCached)
  {
    ldError(422, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported",
            "Operation Not Supported",
            "@context '%s' is of kind 'Cached'; only metadata (?details=true) is available",
            contextId);
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
  // § 6.30.3.1 / § 5.13.4: served as a JSON Object — application/json,
  // not application/ld+json. The body is a JSON-LD context document
  // syntactically but the spec's response type column is "JSON Object",
  // and the ETSI test fixtures pin application/json.
  swRest.out.contentType = (char*) swMimeString(SwMimeJson);

  // A volatile context is a broker-internal, ephemeral Link target whose URL
  // can change across a reap + re-host: tell the fetcher not to store it
  // (Cache-Control: no-store). It is content-addressed and shared across all
  // requests carrying the same inline @context, so it is NOT dropped here —
  // the sliding-TTL reaper retires it once it stops being used.
  if (contextP->volatileCtx)
    swRestOutHeaderAdd("Cache-Control", "no-store");

  swRest.out.httpStatusCode = 200;
  return true;
}

//
// FILE            deleteJsonldContext.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// DELETE /ngsi-ld/v1/jsonldContexts/{contextId}  (NGSI-LD § 5.13.3)
//
// Without ?reload=true: the referenced context is removed from the broker
// cache.
//
// With ?reload=true: the referenced context must be Cached; it is
// re-downloaded and replaces the previous entry. Reload on Implicit or
// Hosted is a 400 Bad Request — there is nothing for the broker to
// re-fetch from.
//

#include <stddef.h>                                    // NULL
#include <string.h>                                    // strcmp

#include "swRest/SwRestState.h"                        // swRest
#include "swJsonld/SwldContext.h"                      // SwldContext, SwldContextKind
#include "swJsonld/SwldContextCache.h"                 // SwldContextCache
#include "swJsonld/swldCache.h"                        // swldCacheLookup, swldCacheRemove, swldCacheInsert
#include "swJsonld/swldDownload.h"                     // swldContextFromUrl
#include "swNgsild/swNgsild.h"                         // ldError, LD_ERROR_*, swNgsild

#include "db/DbDriver.h"                               // db, DB_CONTEXT_KIND_*

#include "serviceRoutines/deleteJsonldContext.h"       // Own interface



// -----------------------------------------------------------------------------
//
// swldCacheGet - internal accessor in swJsonld/swldInit.c
//
extern SwldContextCache* swldCacheGet(void);



// -----------------------------------------------------------------------------
//
// deleteJsonldContext -
//
bool deleteJsonldContext(void)
{
  const char* contextId = swRest.in.wildcard[0];

  if (contextId == NULL || contextId[0] == '\0')
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "context id is missing");
    return true;
  }

  //
  // § 5.13.5.4: a context-id that is not a valid URI shall result in 400
  // BadRequestData. RFC 3986 scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
  // followed by ':'. Cheap-and-correct check: a leading ALPHA, then
  // scheme chars up to a ':'. Anything else (bare word, leading slash, …)
  // is not a URI.
  //
  {
    const char* p = contextId;
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')))
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "context id '%s' is not a valid URI", contextId);
      return true;
    }
    while (*p &&
           ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') ||
             *p == '+' || *p == '-' || *p == '.'))
      p++;
    if (*p != ':')
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "context id '%s' is not a valid URI", contextId);
      return true;
    }
  }

  //
  // reload is parsed via ldParamHook as a boolean (LD_PARAM_RELOAD).
  // We read it via the URL param registry directly to avoid adding yet
  // another SwNgsild state field when this is the only consumer.
  // Strict boolean: only "true"/"false" (case-insensitive); anything else
  // is a malformed URL-param value → 400 (ETSI 051_04_04).
  //
  bool reload = false;
  for (int i = 0; i < swRest.in.uriParamCount; i++)
  {
    if (swRest.in.uriParamV[i].key == NULL ||
        strcmp(swRest.in.uriParamV[i].key, "reload") != 0)
      continue;

    const char* v = swRest.in.uriParamV[i].value;
    if      (v != NULL && strcasecmp(v, "true")  == 0) reload = true;
    else if (v != NULL && strcasecmp(v, "false") == 0) reload = false;
    else
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "'reload' must be a boolean ('true' or 'false')");
      return true;
    }
    break;
  }

  SwldContext* existingP = swldCacheLookup(contextId);
  SwldContext  synth;     // used iff cache miss + DB hit; stack-local lives
                          // until function return, which is past all uses.

  if (existingP == NULL)
  {
    //
    // Cache miss. The entry may still be persisted (LRU-evicted Hosted /
    // Cached). Probe the DB — a plain delete on an evicted row is just a
    // DB delete; for reload we need a SwldContext-shaped handle so the
    // reload branch below can read url+kind off it.
    //
    DbContextRow row = { NULL, NULL, 0, NULL };
    if (db.contextGet == NULL || db.contextGet(contextId, &swRest.kalloc, &row) != DB_OK)
    {
      ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
              "JSON-LD context '%s' not found", contextId);
      return true;
    }

    if (!reload)
    {
      if (db.contextDelete != NULL)
        db.contextDelete(contextId);

      swRest.out.httpStatusCode = 204;
      return true;
    }

    // reload on an evicted entry — only Cached is reloadable.
    if (row.kind != DB_CONTEXT_KIND_CACHED)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "reload is only valid for Cached contexts");
      return true;
    }

    synth.url  = row.url;
    synth.id   = row.id;
    synth.kind = SwldKindCached;
    synth.body = row.body;
    existingP  = &synth;
  }

  if (reload)
  {
    if (existingP->kind != SwldKindCached)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "reload is only valid for Cached contexts");
      return true;
    }

    //
    // Re-download. Detach the old entry first so the fresh download can
    // install itself; if download fails, reinstate the old entry.
    //
    const char* url      = existingP->url;
    KAlloc*     storeP   = swldCacheGet()->kaP;
    SwldContext* removed = swldCacheRemove(contextId);

    SwldContext* fresh = swldContextFromUrl(url, storeP);

    if (fresh == NULL)
    {
      // Put the old one back so the client isn't left with nothing.
      if (removed != NULL)
        swldCacheInsert(removed);

      ldError(504, LD_ERROR_LD_CONTEXT_NOT_AVAILABLE, "Context Not Available",
              "unable to retrieve @context from '%s'", url);
      return true;
    }

    fresh->kind = SwldKindCached;

    //
    // Refresh the persisted body — reload changes the document.
    //
    if (db.contextSave != NULL && fresh->body != NULL && fresh->url != NULL)
      db.contextSave(fresh->url, fresh->url, DB_CONTEXT_KIND_CACHED, fresh->body);

    swRest.out.httpStatusCode = 204;
    return true;
  }

  //
  // Plain delete.
  //
  SwldContextKind removedKind = existingP->kind;
  swldCacheRemove(contextId);

  //
  // Persisted contexts (Hosted, Cached) need to be removed from the DB too.
  // Implicit contexts were never persisted — skip the call.
  //
  if (db.contextDelete != NULL &&
      (removedKind == SwldKindHosted || removedKind == SwldKindCached))
  {
    db.contextDelete(contextId);
  }

  swRest.out.httpStatusCode = 204;
  return true;
}

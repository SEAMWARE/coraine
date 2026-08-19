//
// FILE            deleteJsonldContext.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
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

#include "corRest/CorRestState.h"                        // corRest
#include "corJsonld/CorLdContext.h"                      // CorLdContext, CorLdContextKind
#include "corJsonld/CorLdContextCache.h"                 // CorLdContextCache
#include "corJsonld/corLdCache.h"                        // corLdCacheLookup, corLdCacheRemove, corLdCacheInsert
#include "corJsonld/corLdDownload.h"                     // corLdContextFromUrl, corLdIsCoreContextUrl
#include "corJsonld/corLdInit.h"                         // corLdCoreContext, corLdDownloadGet, CORLD_CORE_CONTEXT_URL
#include "corNgsild/corNgsild.h"                         // ldError, LD_ERROR_*, corNgsild

#include "db/DbDriver.h"                               // db, DB_CONTEXT_KIND_*

#include "serviceRoutines/deleteJsonldContext.h"       // Own interface



// -----------------------------------------------------------------------------
//
// corLdCacheGet / corLdDownloadGet - internal accessors in corJsonld/corLdInit.c
//
extern CorLdContextCache*    corLdCacheGet(void);
extern CorLdDownloadFunction corLdDownloadGet(void);



// -----------------------------------------------------------------------------
//
// deleteJsonldContext -
//
bool deleteJsonldContext(void)
{
  const char* contextId = corRest.in.wildcard[0];

  if (contextId == NULL || contextId[0] == '\0')
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Missing URL Component", "context id is missing");
    return true;
  }

  //
  // No URI shape-check on the contextId — § 13.5.3 calls it a "locally
  // unique identifier", and § 13.5.4 mandates ResourceNotFound for an
  // identifier that "does not correspond to any existing entry", whatever
  // its shape. The lookup below produces the 404. (ETSI 051_02_01,
  // 051_04_02/03.)
  //

  //
  // reload is parsed via ldParamHook as a boolean (LD_PARAM_RELOAD).
  // We read it via the URL param registry directly to avoid adding yet
  // another CorNgsild state field when this is the only consumer.
  // Strict boolean: only "true"/"false" (case-insensitive); anything else
  // is a malformed URL-param value → 400 (ETSI 051_04_04).
  //
  bool reload = false;
  for (int i = 0; i < corRest.in.uriParamCount; i++)
  {
    if (corRest.in.uriParamV[i].key == NULL ||
        strcmp(corRest.in.uriParamV[i].key, "reload") != 0)
      continue;

    const char* v = corRest.in.uriParamV[i].value;
    if      (v != NULL && strcasecmp(v, "true")  == 0) reload = true;
    else if (v != NULL && strcasecmp(v, "false") == 0) reload = false;
    else
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Field Value",
              "'reload' must be a boolean ('true' or 'false')");
      return true;
    }
    break;
  }

  //
  // The Core @context is permanent — the broker cannot operate without it.
  // Any recognised core URL (the configured one, the canonical unversioned
  // form, or an older version — the ignored-stub family) refers to THE core
  // here. A plain delete is refused; ?reload=true re-downloads the broker's
  // configured core to verify availability (the embedded core term tables
  // are canonical and survive either way). ETSI 051_08 / 051_09.
  //
  if (corLdIsCoreContextUrl(contextId))
  {
    if (!reload)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Immutable Field",
              "the Core @context cannot be deleted");
      return true;
    }

    CorLdContext* coreP   = corLdCoreContext();
    const char*  coreUrl = (coreP != NULL && coreP->url != NULL) ? coreP->url : CORLD_CORE_CONTEXT_URL;

    int   downloadStatus = 0;
    char* body           = corLdDownloadGet()(coreUrl, &downloadStatus);

    if (body == NULL)
    {
      ldError(504, LD_ERROR_LD_CONTEXT_NOT_AVAILABLE, "Context Not Available",
              "unable to retrieve @context from '%s'", coreUrl);
      return true;
    }

    corRest.out.httpStatusCode = 204;
    return true;
  }

  CorLdContext* existingP = corLdCacheLookup(contextId);
  CorLdContext  synth;     // used iff cache miss + DB hit; stack-local lives
                          // until function return, which is past all uses.

  if (existingP == NULL)
  {
    //
    // Cache miss. The entry may still be persisted (LRU-evicted Hosted /
    // Cached). Probe the DB — a plain delete on an evicted row is just a
    // DB delete; for reload we need a CorLdContext-shaped handle so the
    // reload branch below can read url+kind off it.
    //
    DbContextRow row = { NULL, NULL, 0, NULL };
    if (db.contextGet == NULL || db.contextGet(contextId, &corRest.kalloc, &row) != DB_OK)
    {
      ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
              "JSON-LD context '%s' not found", contextId);
      return true;
    }

    if (!reload)
    {
      if (db.contextDelete != NULL)
        db.contextDelete(contextId);

      corRest.out.httpStatusCode = 204;
      return true;
    }

    // reload on an evicted entry — only Cached is reloadable.
    if (row.kind != DB_CONTEXT_KIND_CACHED)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Immutable Field",
              "reload is only valid for Cached contexts");
      return true;
    }

    synth.url  = row.url;
    synth.id   = row.id;
    synth.kind = CorLdKindCached;
    synth.body = row.body;
    existingP  = &synth;
  }

  if (reload)
  {
    if (existingP->kind != CorLdKindCached)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Immutable Field",
              "reload is only valid for Cached contexts");
      return true;
    }

    //
    // Re-download. Detach the old entry first so the fresh download can
    // install itself; if download fails, reinstate the old entry.
    //
    const char* url      = existingP->url;
    KAlloc*     storeP   = corLdCacheGet()->kaP;
    CorLdContext* removed = corLdCacheRemove(contextId);

    CorLdContext* fresh = corLdContextFromUrl(url, storeP);

    if (fresh == NULL)
    {
      // Put the old one back so the client isn't left with nothing.
      if (removed != NULL)
        corLdCacheInsert(removed);

      ldError(504, LD_ERROR_LD_CONTEXT_NOT_AVAILABLE, "Context Not Available",
              "unable to retrieve @context from '%s'", url);
      return true;
    }

    fresh->kind = CorLdKindCached;

    //
    // Refresh the persisted body — reload changes the document.
    //
    if (db.contextSave != NULL && fresh->body != NULL && fresh->url != NULL)
      db.contextSave(fresh->url, fresh->url, DB_CONTEXT_KIND_CACHED, fresh->body);

    corRest.out.httpStatusCode = 204;
    return true;
  }

  //
  // Plain delete.
  //
  CorLdContextKind removedKind = existingP->kind;
  corLdCacheRemove(contextId);

  //
  // Persisted contexts (Hosted, Cached) need to be removed from the DB too.
  // Implicit contexts were never persisted — skip the call.
  //
  if (db.contextDelete != NULL &&
      (removedKind == CorLdKindHosted || removedKind == CorLdKindCached))
  {
    db.contextDelete(contextId);
  }

  corRest.out.httpStatusCode = 204;
  return true;
}

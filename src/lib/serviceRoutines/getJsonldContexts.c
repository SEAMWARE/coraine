//
// FILE            getJsonldContexts.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/jsonldContexts - Retrieve Available JSON-LD Contexts.
// NGSI-LD v1.9.1 § 5.13.5.
//
// URL parameters:
//   details  — true/false. false (default): array of context URLs (strings).
//              true: array of objects with kind/createdAt/lastUsage detail.
//   kind     — "Hosted" | "Cached" | "ImplicitlyCreated". Filter.
//   limit    — pagination limit (default 20, capped in hooks).
//   offset   — pagination offset.
//   count    — true => emit NGSILD-Results-Count header with pre-pagination total.
//

#include <stddef.h>                                    // NULL
#include <stdio.h>                                     // snprintf
#include <string.h>                                    // strcmp, strlen, memcpy
#include <time.h>                                      // gmtime_r, struct tm

#include "corRest/CorRestState.h"                        // corRest
#include "corRest/corRestOutHeader.h"                   // corRestOutHeaderAdd
#include "kalloc/kaAlloc.h"                            // kaAlloc
#include "kjson/kjBuilder.h"                           // kjObject, kjArray, kjString, kjInteger, kjChildAdd
#include "corJsonld/CorLdContext.h"                      // CorLdContext, CorLdContextKind
#include "corJsonld/corLdCache.h"                        // corLdCacheSnapshot
#include "corNgsild/corNgsild.h"                         // corNgsild
#include "corNgsild/CorNgsild.h"                         // ldBrokerHttpEndpoint

#include "serviceRoutines/getJsonldContexts.h"         // Own interface



// -----------------------------------------------------------------------------
//
// kindString - NGSI-LD § 5.13.1 kind name
//
static const char* kindString(CorLdContextKind k)
{
  switch (k)
  {
    case CorLdKindHosted:   return "Hosted";
    case CorLdKindCached:   return "Cached";
    case CorLdKindImplicit: return "ImplicitlyCreated";
  }
  return "ImplicitlyCreated";
}



// -----------------------------------------------------------------------------
//
// getJsonldContexts -
//
bool getJsonldContexts(void)
{
  CorLdContext** arr = NULL;
  int           n   = 0;

  corLdCacheSnapshot(&corRest.kalloc, &arr, &n);

  //
  // Apply ?kind= filter (if specified and valid; unknown values filter out
  // everything rather than 400 — matches spec's treatment of enum params).
  //
  CorLdContextKind wantKind   = CorLdKindImplicit;
  bool            kindFilter = false;

  if (corNgsild.kind != NULL)
  {
    kindFilter = true;
    if      (strcmp(corNgsild.kind, "Hosted")            == 0) wantKind = CorLdKindHosted;
    else if (strcmp(corNgsild.kind, "Cached")            == 0) wantKind = CorLdKindCached;
    else if (strcmp(corNgsild.kind, "ImplicitlyCreated") == 0) wantKind = CorLdKindImplicit;
    else
    {
      // Unknown kind: empty result
      wantKind = (CorLdContextKind) -1;
    }
  }


  //
  // Count filtered items for NGSILD-Results-Count header, then assemble the
  // paginated response array.
  //
  int totalCount = 0;
  for (int i = 0; i < n; i++)
  {
    if (arr[i]->volatileCtx)   // one-shot Link targets are not listable contexts
      continue;
    if (kindFilter && arr[i]->kind != wantKind)
      continue;
    totalCount++;
  }

  if (corNgsild.count)
  {
    char* countStr = (char*) kaAlloc(&corRest.kalloc, 32);
    snprintf(countStr, 32, "%d", totalCount);

    corRestOutHeaderAdd("NGSILD-Results-Count", countStr);
  }

  //
  // Build response array — honouring offset/limit over the filtered view.
  //
  KjNode* arrayP = kjArray(corRest.kjsonP, NULL);
  int     skipN  = (corNgsild.offset > 0) ? corNgsild.offset : 0;
  int     limit  = (corNgsild.limit > 0) ? corNgsild.limit : totalCount;
  int     taken  = 0;
  int     seen   = 0;

  for (int i = 0; i < n; i++)
  {
    CorLdContext* c = arr[i];

    if (c->volatileCtx)   // one-shot Link targets are not listable contexts
      continue;

    if (kindFilter && c->kind != wantKind)
      continue;

    if (seen < skipN)
    {
      seen++;
      continue;
    }

    if (taken >= limit)
      break;

    taken++;
    seen++;

    const char* contextId = (c->id != NULL) ? c->id : c->url;
    if (contextId == NULL)
      continue;

    // § 5.13.3.5: the no-details response is a list of URLs. For Cached,
    // c->url is the original download URL. For Hosted / ImplicitlyCreated
    // there is no source URL — the URL where the broker serves them is
    // <httpEndpoint>/ngsi-ld/v1/jsonldContexts/{localId}, so we synthesise
    // it here. (Same shape as the URL field on the ?details=true entries.)
    const char* urlOut = c->url;
    if (urlOut == NULL)
    {
      const char* prefix = "/ngsi-ld/v1/jsonldContexts/";
      const char* base   = (ldBrokerHttpEndpoint != NULL) ? ldBrokerHttpEndpoint : "";
      int   baseLen      = strlen(base);
      int   prefixLen    = strlen(prefix);
      int   idLen        = strlen(contextId);
      char* buf          = (char*) kaAlloc(&corRest.kalloc, baseLen + prefixLen + idLen + 1);
      memcpy(buf, base, baseLen);
      memcpy(buf + baseLen, prefix, prefixLen);
      memcpy(buf + baseLen + prefixLen, contextId, idLen);
      buf[baseLen + prefixLen + idLen] = 0;
      urlOut = buf;
    }

    if (!corNgsild.details)
    {
      kjChildAdd(arrayP, kjString(corRest.kjsonP, NULL, (char*) urlOut));
    }
    else
    {
      // § 5.13.3.5 metadata field names (NB. spec uses URL/localId, not
      // url/id; kind is one of Hosted/Cached/ImplicitlyCreated).
      KjNode* obj = kjObject(corRest.kjsonP, NULL);
      kjChildAdd(obj, kjString(corRest.kjsonP, "URL",       (char*) urlOut));
      kjChildAdd(obj, kjString(corRest.kjsonP, "localId",   contextId));
      kjChildAdd(obj, kjString(corRest.kjsonP, "kind",      kindString(c->kind)));
      // § 5.13.3.5: DateTime strings, not Unix timestamps.
      {
        time_t  secs;
        struct  tm tm;
        char*   ca = (char*) kaAlloc(&corRest.kalloc, 80);
        secs = (time_t) c->createdAt;
        gmtime_r(&secs, &tm);
        snprintf(ca, 80, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        kjChildAdd(obj, kjString(corRest.kjsonP, "createdAt", ca));

        char* lu = (char*) kaAlloc(&corRest.kalloc, 80);
        secs = (time_t) c->usedAt;
        gmtime_r(&secs, &tm);
        snprintf(lu, 80, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        kjChildAdd(obj, kjString(corRest.kjsonP, "lastUsage", lu));
      }
      kjChildAdd(arrayP, obj);
    }
  }

  corNgsild.rawResponse    = true;
  corRest.out.responseTree = arrayP;
  return true;
}

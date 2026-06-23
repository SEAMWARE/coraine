//
// FILE            getJsonldContexts.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
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

#include "swRest/SwRestState.h"                        // swRest
#include "swRest/swRestOutHeader.h"                   // swRestOutHeaderAdd
#include "kalloc/kaAlloc.h"                            // kaAlloc
#include "kjson/kjBuilder.h"                           // kjObject, kjArray, kjString, kjInteger, kjChildAdd
#include "swJsonld/SwldContext.h"                      // SwldContext, SwldContextKind
#include "swJsonld/swldCache.h"                        // swldCacheSnapshot
#include "swNgsild/swNgsild.h"                         // swNgsild
#include "swNgsild/SwNgsild.h"                         // ldBrokerHttpEndpoint

#include "serviceRoutines/getJsonldContexts.h"         // Own interface



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
// getJsonldContexts -
//
bool getJsonldContexts(void)
{
  SwldContext** arr = NULL;
  int           n   = 0;

  swldCacheSnapshot(&swRest.kalloc, &arr, &n);

  //
  // Apply ?kind= filter (if specified and valid; unknown values filter out
  // everything rather than 400 — matches spec's treatment of enum params).
  //
  SwldContextKind wantKind   = SwldKindImplicit;
  bool            kindFilter = false;

  if (swNgsild.kind != NULL)
  {
    kindFilter = true;
    if      (strcmp(swNgsild.kind, "Hosted")            == 0) wantKind = SwldKindHosted;
    else if (strcmp(swNgsild.kind, "Cached")            == 0) wantKind = SwldKindCached;
    else if (strcmp(swNgsild.kind, "ImplicitlyCreated") == 0) wantKind = SwldKindImplicit;
    else
    {
      // Unknown kind: empty result
      wantKind = (SwldContextKind) -1;
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

  if (swNgsild.count)
  {
    char* countStr = (char*) kaAlloc(&swRest.kalloc, 32);
    snprintf(countStr, 32, "%d", totalCount);

    swRestOutHeaderAdd("NGSILD-Results-Count", countStr);
  }

  //
  // Build response array — honouring offset/limit over the filtered view.
  //
  KjNode* arrayP = kjArray(swRest.kjsonP, NULL);
  int     skipN  = (swNgsild.offset > 0) ? swNgsild.offset : 0;
  int     limit  = (swNgsild.limit > 0) ? swNgsild.limit : totalCount;
  int     taken  = 0;
  int     seen   = 0;

  for (int i = 0; i < n; i++)
  {
    SwldContext* c = arr[i];

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
      char* buf          = (char*) kaAlloc(&swRest.kalloc, baseLen + prefixLen + idLen + 1);
      memcpy(buf, base, baseLen);
      memcpy(buf + baseLen, prefix, prefixLen);
      memcpy(buf + baseLen + prefixLen, contextId, idLen);
      buf[baseLen + prefixLen + idLen] = 0;
      urlOut = buf;
    }

    if (!swNgsild.details)
    {
      kjChildAdd(arrayP, kjString(swRest.kjsonP, NULL, (char*) urlOut));
    }
    else
    {
      // § 5.13.3.5 metadata field names (NB. spec uses URL/localId, not
      // url/id; kind is one of Hosted/Cached/ImplicitlyCreated).
      KjNode* obj = kjObject(swRest.kjsonP, NULL);
      kjChildAdd(obj, kjString(swRest.kjsonP, "URL",       (char*) urlOut));
      kjChildAdd(obj, kjString(swRest.kjsonP, "localId",   contextId));
      kjChildAdd(obj, kjString(swRest.kjsonP, "kind",      kindString(c->kind)));
      // § 5.13.3.5: DateTime strings, not Unix timestamps.
      {
        time_t  secs;
        struct  tm tm;
        char*   ca = (char*) kaAlloc(&swRest.kalloc, 80);
        secs = (time_t) c->createdAt;
        gmtime_r(&secs, &tm);
        snprintf(ca, 80, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        kjChildAdd(obj, kjString(swRest.kjsonP, "createdAt", ca));

        char* lu = (char*) kaAlloc(&swRest.kalloc, 80);
        secs = (time_t) c->usedAt;
        gmtime_r(&secs, &tm);
        snprintf(lu, 80, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        kjChildAdd(obj, kjString(swRest.kjsonP, "lastUsage", lu));
      }
      kjChildAdd(arrayP, obj);
    }
  }

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = arrayP;
  return true;
}

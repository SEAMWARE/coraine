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
#include <string.h>                                    // strcmp

#include "swRest/SwRestState.h"                        // swRest
#include "swRest/swRestOutHeader.h"                   // swRestOutHeaderAdd
#include "kalloc/kaAlloc.h"                            // kaAlloc
#include "kjson/kjBuilder.h"                           // kjObject, kjArray, kjString, kjInteger, kjChildAdd
#include "swJsonld/SwldContext.h"                      // SwldContext, SwldContextKind
#include "swJsonld/swldCache.h"                        // swldCacheSnapshot
#include "swNgsild/swNgsild.h"                         // swNgsild

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
  KjNode* arrayP = kjArray(NULL, NULL);
  int     skipN  = (swNgsild.offset > 0) ? swNgsild.offset : 0;
  int     limit  = (swNgsild.limit > 0) ? swNgsild.limit : totalCount;
  int     taken  = 0;
  int     seen   = 0;

  for (int i = 0; i < n; i++)
  {
    SwldContext* c = arr[i];

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

    if (!swNgsild.details)
    {
      kjChildAdd(arrayP, kjString(NULL, NULL, contextId));
    }
    else
    {
      KjNode* obj = kjObject(NULL, NULL);
      kjChildAdd(obj, kjString(NULL, "id",        contextId));
      kjChildAdd(obj, kjString(NULL, "kind",      kindString(c->kind)));
      if (c->url != NULL)
        kjChildAdd(obj, kjString(NULL, "url",       c->url));
      kjChildAdd(obj, kjInteger(NULL, "createdAt", (long long) c->createdAt));
      kjChildAdd(obj, kjInteger(NULL, "lastUsage", (long long) c->usedAt));
      kjChildAdd(arrayP, obj);
    }
  }

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = arrayP;
  return true;
}

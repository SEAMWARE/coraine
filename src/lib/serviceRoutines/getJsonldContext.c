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

#include <stddef.h>                                    // NULL
#include <string.h>                                    // strlen

#include "swRest/SwRestState.h"                        // swRest
#include "swJsonld/SwldContext.h"                      // SwldContext
#include "swJsonld/swldCache.h"                        // swldCacheLookup
#include "swNgsild/swNgsild.h"                         // ldError, LD_ERROR_*, swNgsild

#include "serviceRoutines/getJsonldContext.h"          // Own interface



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
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "JSON-LD context '%s' not found", contextId);
    return true;
  }

  if (contextP->body == NULL)
  {
    //
    // Cached entry exists but its body was not preserved (e.g. loaded from
    // a path that predates body capture). Return 404 rather than fabricating
    // a body from the parsed form, which would be lossy.
    //
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "JSON-LD context '%s' body not available", contextId);
    return true;
  }

  //
  // Raw body response — bypass the JSON tree renderer by using the
  // payload/payloadSize escape hatch, and set Content-Type to ld+json.
  //
  swRest.out.payload     = contextP->body;
  swRest.out.payloadSize = strlen(contextP->body);
  swRest.out.contentType = (char*) "application/ld+json";

  swRest.out.httpStatusCode = 200;
  return true;
}

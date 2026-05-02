//
// FILE            getVersion.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /version — broker product / version handshake.
//
// Returns a small JSON object with the swBroker product name and version
// plus the linked swNgsild / swJsonld library versions. Useful as a
// liveness probe and as a deployment-version verification endpoint.
//
#include <stdbool.h>                                 // bool

#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjString, kjChildAdd

#include "swRest/SwRestState.h"                      // swRest

#include "swNgsild/swNgsild.h"                       // SWNGSILD_VERSION
#include "swJsonld/swJsonld.h"                       // SWJSONLD_VERSION

#include "swBrokerVersion.h"                         // SWBROKER_VERSION (-Isrc/app/swBroker)
#include "serviceRoutines/getVersion.h"              // Own interface



bool getVersion(void)
{
  KjNode* body = kjObject(swRest.kjsonP, NULL);

  kjChildAdd(body, kjString(swRest.kjsonP, "product",         "swBroker"));
  kjChildAdd(body, kjString(swRest.kjsonP, "version",         SWBROKER_VERSION));
  kjChildAdd(body, kjString(swRest.kjsonP, "swNgsildVersion", SWNGSILD_VERSION));
  kjChildAdd(body, kjString(swRest.kjsonP, "swJsonldVersion", SWJSONLD_VERSION));

  // Bypass @context expansion / compaction — this endpoint is non-NGSI-LD.
  swNgsild.rawResponse      = true;
  swRest.out.responseTree   = body;
  swRest.out.httpStatusCode = 200;
  return true;
}

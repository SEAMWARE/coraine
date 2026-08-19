//
// FILE            getVersion.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /version — broker product / version handshake.
//
// Returns a small JSON object with the coraine product name and version
// plus the linked corNgsild / corJsonld library versions. Useful as a
// liveness probe and as a deployment-version verification endpoint.
//
#include <stdbool.h>                                 // bool

#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjString, kjChildAdd

#include "corRest/CorRestState.h"                      // corRest

#include "corNgsild/corNgsild.h"                       // CORNGSILD_VERSION
#include "corJsonld/corJsonld.h"                       // CORJSONLD_VERSION

#include "coraineVersion.h"                         // CORAINE_VERSION (-Isrc/app/coraine)
#include "serviceRoutines/getVersion.h"              // Own interface



bool getVersion(void)
{
  KjNode* body = kjObject(corRest.kjsonP, NULL);

  kjChildAdd(body, kjString(corRest.kjsonP, "product",         "coraine"));
  kjChildAdd(body, kjString(corRest.kjsonP, "version",         CORAINE_VERSION));
  kjChildAdd(body, kjString(corRest.kjsonP, "corNgsildVersion", CORNGSILD_VERSION));
  kjChildAdd(body, kjString(corRest.kjsonP, "corJsonldVersion", CORJSONLD_VERSION));

  // Bypass @context expansion / compaction — this endpoint is non-NGSI-LD.
  corNgsild.rawResponse      = true;
  corRest.out.responseTree   = body;
  corRest.out.httpStatusCode = 200;
  return true;
}

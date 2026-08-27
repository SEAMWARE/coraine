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
#include "corNgsild/corNgsild.h"                       // corNgsild (rawResponse)

#include "coraineVersion.h"                         // CORAINE_VERSION (-Isrc/app/coraine)
#include "coraineStack.h"                           // coraineStack - GENERATED, see the makefile
#include "serviceRoutines/getVersion.h"              // Own interface



bool getVersion(void)
{
  KjNode* body = kjObject(corRest.kjsonP, NULL);

  kjChildAdd(body, kjString(corRest.kjsonP, "product", "coraine"));
  kjChildAdd(body, kjString(corRest.kjsonP, "version", CORAINE_VERSION));

  //
  // The libraries are most of this binary, none of them is released, and the
  // cor* repos track main - so coraine's own version answers only part of
  // "what am I talking to". The commit of each one, resolved at build time,
  // answers the rest exactly.
  //
  // Commits rather than version strings on purpose: the hand-maintained
  // #defines this replaced said things like "post-0.2.0", which is a promise
  // that something happened after 0.2.0 and no help to anyone holding a bug.
  //
  KjNode* stack = kjObject(corRest.kjsonP, "stack");

  for (int ix = 0; coraineStack[ix][0] != NULL; ix++)
    kjChildAdd(stack, kjString(corRest.kjsonP, coraineStack[ix][0], coraineStack[ix][1]));

  kjChildAdd(body, stack);

  // Bypass @context expansion / compaction — this endpoint is non-NGSI-LD.
  corNgsild.rawResponse      = true;
  corRest.out.responseTree   = body;
  corRest.out.httpStatusCode = 200;
  return true;
}

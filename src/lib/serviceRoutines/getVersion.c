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

#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeBuilder.h"                  // corTreeObject, corTreeString, corTreeChildAdd

#include "corRest/CorRestState.h"                      // corRest
#include "corNgsild/corNgsild.h"                       // corNgsild (rawResponse)
#include "corBridge/BridgeDriver.h"                     // bridges, bridgeCount

#include "coraineVersion.h"                         // CORAINE_VERSION (-Isrc/app/coraine)
#include "coraineStack.h"                           // coraineStack - GENERATED, see the makefile
#include "serviceRoutines/getVersion.h"              // Own interface



bool getVersion(void)
{
  CorNode* body = corTreeObject(corRest.kallocP, NULL);

  corTreeChildAdd(body, corTreeString(corRest.kallocP, "product", "coraine"));
  corTreeChildAdd(body, corTreeString(corRest.kallocP, "version", CORAINE_VERSION));

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
  CorNode* stack = corTreeObject(corRest.kallocP, "stack");

  for (int ix = 0; coraineStack[ix][0] != NULL; ix++)
    corTreeChildAdd(stack, corTreeString(corRest.kallocP, coraineStack[ix][0], coraineStack[ix][1]));

  corTreeChildAdd(body, stack);

  //
  // The bridges, one member per loaded plugin, its value the plugin's own
  // one-liner - BridgeDriver.h promises that string to this response.
  //
  // ⭐ A bridge plugin links a transport stack built apart from coraine (DDS is
  // five times the size of the whole broker), so the commits above say nothing
  // about it; only the plugin can say what it actually loaded.
  //
  // Absent rather than empty when no bridge is loaded: a broker without
  // --bridges answers exactly as it always did.
  //
  if (bridgeCount > 0)
  {
    CorNode* bridgesP = corTreeObject(corRest.kallocP, "bridges");

    for (int ix = 0; ix < bridgeCount; ix++)
    {
      const char* alias = (bridges[ix].alias       != NULL) ? bridges[ix].alias         : "?";
      const char* info  = (bridges[ix].versionInfo != NULL) ? bridges[ix].versionInfo() : "";

      corTreeChildAdd(bridgesP, corTreeString(corRest.kallocP, alias, info));
    }

    corTreeChildAdd(body, bridgesP);
  }

  // Bypass @context expansion / compaction — this endpoint is non-NGSI-LD.
  corNgsild.rawResponse      = true;
  corRest.out.responseTree   = body;
  corRest.out.httpStatusCode = 200;
  return true;
}

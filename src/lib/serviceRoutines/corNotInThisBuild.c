//
// FILE            corNotInThisBuild.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                 // bool

#include "corRest/CorRestState.h"                      // corRest
#include "corNgsild/ldError.h"                         // ldError

#include "serviceRoutines/corNotInThisBuild.h"       // Own interface



// -----------------------------------------------------------------------------
//
// corNotInThisBuild -
//
bool corNotInThisBuild(void)
{
  //
  // The route EXISTS - it is in the service table, it matched, and the same URL
  // on a full build would work. That is why this is not a 404: a 404 says the
  // resource is not there, and invites the client to fix its URL. 501 plus the
  // verb and path says the deployment declined the capability, and the client's
  // move is a different deployment, not a different request.
  //
  ldError(501, LD_ERROR_NOT_IN_THIS_BUILD, "Not Available In This Build",
          "'%s %s' is not included in this build of coraine",
          corRest.in.verbString, corRest.in.urlPath);

  return true;
}

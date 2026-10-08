//
// FILE            startupContext.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdlib.h>                                       // malloc
#include <string.h>                                       // memcpy

#include "corRest/corRestClient.h"                        // corRestClient*, CorRestClientRequest/Response

#include "corNgsild/CorNgsild.h"                          // corNgsild
#include "corNgsild/ldError.h"                            // ldError
#include "corNgsild/LdProblem.h"                          // LD_ERROR_BAD_REQUEST_DATA, LD_ERROR_LD_CONTEXT_NOT_AVAILABLE

#include "startup/startupContext.h"                       // Own interface



// -----------------------------------------------------------------------------
//
// startupContextDownload -
//
char* startupContextDownload(const char* url, int* statusCodeP)
{
  CorRestClientRequest  req;
  CorRestClientResponse resp;

  corRestClientRequestInit(&req, CorVerbGet, url, NULL);
  corRestClientRequestHeader(&req, "Accept", "application/ld+json, application/json");
  corRestClientRequestTimeout(&req, 5000, 10000);

  int r = corRestClientSend(&req, &resp);

  if (r != CORR_OK || resp.statusCode != 200)
  {
    *statusCodeP = (resp.statusCode > 0) ? resp.statusCode : 500;
    corRestClientResponseCleanup(&resp);
    return NULL;
  }

  *statusCodeP = 200;

  // Return a malloc'd copy of the body (corJsonld will free it)
  char* copy = NULL;
  if (resp.body != NULL && resp.bodyLen > 0)
  {
    copy = (char*) malloc(resp.bodyLen + 1);
    memcpy(copy, resp.body, resp.bodyLen);
    copy[resp.bodyLen] = 0;
  }

  corRestClientResponseCleanup(&resp);
  return copy;
}



// -----------------------------------------------------------------------------
//
// startupContextError -
//
void startupContextError(int status, const char* title, const char* detail)
{
  //
  // 501: a JSON-LD feature this broker does not implement (@import). TS 104-176 registers no error type
  // for that - only NoMultiTenantSupport says "not implemented", for one feature (spec-doubts-2 #137) -
  // so the type is ours, as for NotAvailableInThisDeployment.
  //
  const char* type = (status == 400) ? LD_ERROR_BAD_REQUEST_DATA
                   : (status == 501) ? "https://coraine.readthedocs.io/errors/NotImplemented"
                   :                   LD_ERROR_LD_CONTEXT_NOT_AVAILABLE;

  ldError(status, type, title, "%s", detail);
  corNgsild.contextError = true;
}

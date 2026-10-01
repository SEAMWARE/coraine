//
// FILE            forwardingCor.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The cor:// forwarding plugin - doc/cor-protocol.md § 5.
//
// A registration whose endpoint is cor://host:port is forwarded over corRest's cor client: the
// request goes as a tree, and the answer comes back as one (LdForwardResponse.bodyTree), so neither
// broker renders or parses JSON for it.
//
#include <stddef.h>                                    // NULL
#include <string.h>                                    // strncpy, strchr
#include <time.h>                                      // clock_gettime

#include "corLog/corLog.h"                             // COR_E
#include "corRest/corRestCor.h"                        // corRestCorSend, CorRestCorResponse
#include "corNgsild/LdForwarding.h"                    // LdForwardRequest, LdForwardResponse, LdForwardingPlugin
#include "corNgsild/ldForwarding.h"                    // ldForwardingRegister

#include "metrics/metrics.h"                           // metricsDistopForward

#include "forwarding/forwardingCor.h"                  // Own interface



// -----------------------------------------------------------------------------
//
// corSend - the LdForwardSendFunc for cor://
//
// req->endpoint is the whole URL - cor://host:port/ngsi-ld/v1/... - and the path is what follows the
// authority.
//
static int corSend(LdForwardRequest* req, LdForwardResponse* resp)
{
  if ((req == NULL) || (resp == NULL) || (resp->allocP == NULL))
    return -1;

  const char* authorityP = (strncmp(req->endpoint, "cor://", 6) == 0) ? &req->endpoint[6] : req->endpoint;
  const char* pathP      = strchr(authorityP, '/');

  if (pathP == NULL)
    pathP = "/";

  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  CorRestCorResponse cResp;
  const char*        error = NULL;
  bool               ok    = corRestCorSend(req->endpoint, req->verb, pathP, req->headerV, req->headerCount,
                                            NULL, req->body, req->requestTimeoutMs, resp->allocP, &cResp, &error);

  struct timespec t1;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double latency = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

  if (ok == false)
  {
    metricsDistopForward(latency, false);
    resp->error      = -1;
    resp->statusCode = 0;
    strncpy(resp->errorDetail, (error != NULL) ? error : "cor:// forwarding failed", sizeof(resp->errorDetail) - 1);
    resp->errorDetail[sizeof(resp->errorDetail) - 1] = 0;
    return resp->error;
  }

  metricsDistopForward(latency, (cResp.status >= 200) && (cResp.status < 300));

  resp->error       = 0;
  resp->statusCode  = cResp.status;
  resp->headerV     = cResp.headerV;                 // all of it in resp->allocP already
  resp->headerCount = cResp.headerCount;
  resp->bodyTree    = cResp.bodyTree;
  resp->body        = cResp.bodyText;
  resp->bodyLen     = (cResp.bodyText != NULL) ? (int) strlen(cResp.bodyText) : 0;

  return 0;
}



// -----------------------------------------------------------------------------
//
// Plugin descriptor
//
static const char* const corSchemes[] = { "cor", NULL };

static const LdForwardingPlugin corPlugin =
{
  .alias   = "cor",
  .schemes = corSchemes,
  .send    = corSend
};



// -----------------------------------------------------------------------------
//
// forwardingCorRegister -
//
void forwardingCorRegister(void)
{
  if (ldForwardingRegister(&corPlugin) == false)
    COR_E("forwarding: failed to register the cor:// plugin (already claimed?)");
}

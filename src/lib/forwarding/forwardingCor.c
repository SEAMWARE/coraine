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
// broker renders or parses JSON for it. In a fan-out, every cor:// forward is started (corStart)
// before any is waited for (corWait).
//
#include <stddef.h>                                    // NULL
#include <stdio.h>                                     // snprintf
#include <stdlib.h>                                    // malloc, free
#include <string.h>                                    // strncpy, strchr
#include <time.h>                                      // clock_gettime

#include "corLog/corLog.h"                             // COR_E
#include "corRest/corRestCor.h"                        // corRestCorSend, corRestCorStart, corRestCorWait, CorRestCorResponse
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
// CorForward - a forward started (corStart), to be waited for (corWait)
//
typedef struct CorForward
{
  CorRestCorCall*  callP;
  struct timespec  t0;
} CorForward;



// -----------------------------------------------------------------------------
//
// corStart - the LdForwardStartFunc for cor://: sent now, the response collected by corWait
//
static void* corStart(LdForwardRequest* req, CorAlloc* kaP, char* errorDetail, int errorDetailSize)
{
  const char* authorityP = (strncmp(req->endpoint, "cor://", 6) == 0) ? &req->endpoint[6] : req->endpoint;
  const char* pathP      = strchr(authorityP, '/');
  CorForward* fP         = (CorForward*) malloc(sizeof(CorForward));
  const char* error      = NULL;

  if (fP == NULL)
  {
    snprintf(errorDetail, errorDetailSize, "out of memory");
    return NULL;
  }

  clock_gettime(CLOCK_MONOTONIC, &fP->t0);

  fP->callP = corRestCorStart(req->endpoint, req->verb, (pathP != NULL) ? pathP : "/", req->headerV, req->headerCount,
                              NULL, req->body, req->requestTimeoutMs, kaP, &error);

  if (fP->callP == NULL)
  {
    snprintf(errorDetail, errorDetailSize, "%s", (error != NULL) ? error : "cor:// forwarding failed");
    free(fP);
    return NULL;
  }

  return fP;
}



// -----------------------------------------------------------------------------
//
// corWait - the LdForwardWaitFunc for cor://
//
static int corWait(void* handle, LdForwardResponse* resp)
{
  CorForward*        fP = (CorForward*) handle;
  CorRestCorResponse cResp;
  const char*        error = NULL;
  bool               ok    = corRestCorWait(fP->callP, resp->allocP, &cResp, &error);

  struct timespec t1;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double latency = (t1.tv_sec - fP->t0.tv_sec) + (t1.tv_nsec - fP->t0.tv_nsec) / 1e9;

  free(fP);

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
  resp->headerV     = cResp.headerV;
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
  .send    = corSend,
  .start   = corStart,
  .wait    = corWait
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

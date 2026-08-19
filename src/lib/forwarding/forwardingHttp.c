//
// FILE            forwardingHttp.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// HTTP / HTTPS forwarding plugin. Translates an LdForwardRequest into
// a corRest CorRestClientRequest, sends it synchronously, and copies the
// CorRestClientResponse fields back into the LdForwardResponse the
// caller-supplied arena.
//
#include <stddef.h>                                    // NULL
#include <string.h>                                    // strncpy
#include <time.h>                                      // clock_gettime

#include "ktrace/kTrace.h"                             // KT_E
#include "kalloc/KAlloc.h"                             // KAlloc
#include "kalloc/kaAlloc.h"                            // kaAlloc

#include "corRest/corRestClient.h"                       // CorRestClientRequest, corRestClientSend, ...
#include "corNgsild/LdForwarding.h"                     // LdForwardRequest, LdForwardResponse, LdForwardingPlugin
#include "corNgsild/ldForwarding.h"                     // ldForwardingRegister

#include "metrics/metrics.h"                           // metricsDistopForward

#include "forwarding/forwardingHttp.h"                 // Own interface



// -----------------------------------------------------------------------------
//
// strdupInto - copy a NUL-terminated string into the response arena
//
static char* strdupInto(KAlloc* allocP, const char* s)
{
  if (s == NULL) return NULL;

  int len = 0;
  while (s[len] != 0) len++;

  char* dst = (char*) kaAlloc(allocP, len + 1);
  if (dst == NULL) return NULL;

  for (int i = 0; i <= len; i++)
    dst[i] = s[i];

  return dst;
}



// -----------------------------------------------------------------------------
//
// memdupInto - copy a byte buffer (possibly with embedded NULs) into the arena
//
static char* memdupInto(KAlloc* allocP, const char* src, int len)
{
  if (src == NULL || len <= 0) return NULL;

  char* dst = (char*) kaAlloc(allocP, len + 1);
  if (dst == NULL) return NULL;

  for (int i = 0; i < len; i++)
    dst[i] = src[i];
  dst[len] = 0;

  return dst;
}



// -----------------------------------------------------------------------------
//
// httpSend - the LdForwardSendFunc for HTTP / HTTPS
//
static int httpSend(LdForwardRequest* req, LdForwardResponse* resp)
{
  if (req == NULL || resp == NULL || resp->allocP == NULL)
    return -1;

  CorRestClientRequest  cReq;
  CorRestClientResponse cResp;

  corRestClientRequestInit(&cReq, req->verb, req->endpoint, resp->allocP);

  for (int i = 0; i < req->headerCount; i++)
  {
    if (req->headerV[i].key != NULL && req->headerV[i].value != NULL)
      corRestClientRequestHeader(&cReq, req->headerV[i].key, req->headerV[i].value);
  }

  if (req->body != NULL && req->bodyLen > 0)
    corRestClientRequestBody(&cReq, req->body, req->bodyLen);

  if (req->connectTimeoutMs > 0 || req->requestTimeoutMs > 0)
    corRestClientRequestTimeout(&cReq, req->connectTimeoutMs, req->requestTimeoutMs);

  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  int rc = corRestClientSend(&cReq, &cResp);

  struct timespec t1;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double latency = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

  if (rc != 0 || cResp.error != 0)
  {
    metricsDistopForward(latency, false);
    resp->error      = (cResp.error != 0) ? cResp.error : rc;
    resp->statusCode = 0;
    strncpy(resp->errorDetail, cResp.errorDetail[0] != 0 ? cResp.errorDetail : "http forwarding failed",
            sizeof(resp->errorDetail) - 1);
    resp->errorDetail[sizeof(resp->errorDetail) - 1] = 0;
    corRestClientResponseCleanup(&cResp);
    return resp->error;
  }

  metricsDistopForward(latency, cResp.statusCode >= 200 && cResp.statusCode < 300);

  resp->error      = 0;
  resp->statusCode = cResp.statusCode;
  resp->bodyLen    = cResp.bodyLen;
  resp->body       = (cResp.bodyLen > 0) ? memdupInto(resp->allocP, cResp.body, cResp.bodyLen) : NULL;

  // Copy headers — keys + values both into the arena
  resp->headerCount = cResp.headerCount;
  if (cResp.headerCount > 0)
  {
    resp->headerV = (CorRestKeyValue*) kaAlloc(resp->allocP, cResp.headerCount * sizeof(CorRestKeyValue));
    for (int i = 0; i < cResp.headerCount; i++)
    {
      resp->headerV[i].key   = strdupInto(resp->allocP, cResp.headerV[i].key);
      resp->headerV[i].value = strdupInto(resp->allocP, cResp.headerV[i].value);
    }
  }
  else
  {
    resp->headerV = NULL;
  }

  corRestClientResponseCleanup(&cResp);   // upstream headers copied above; free the client response's vector
  return 0;
}



// -----------------------------------------------------------------------------
//
// Plugin descriptor
//
static const char* const httpSchemes[] = { "http", "https", NULL };

static const LdForwardingPlugin httpPlugin =
{
  .alias   = "http",
  .schemes = httpSchemes,
  .send    = httpSend
};



// -----------------------------------------------------------------------------
//
// forwardingHttpRegister -
//
void forwardingHttpRegister(void)
{
  if (ldForwardingRegister(&httpPlugin) == false)
    KT_E("forwarding: failed to register HTTP plugin (already claimed?)");
}

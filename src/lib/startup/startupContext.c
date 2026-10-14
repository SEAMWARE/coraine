//
// FILE            startupContext.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdlib.h>                                       // malloc, strtol
#include <string.h>                                       // memcpy, strncmp, strchr, strlen

#include "corRest/corRestClient.h"                        // corRestClient*, CorRestClientRequest/Response

#include "corNgsild/CorNgsild.h"                          // corNgsild
#include "corNgsild/ldError.h"                            // ldError
#include "corNgsild/LdProblem.h"                          // LD_ERROR_BAD_REQUEST_DATA, LD_ERROR_LD_CONTEXT_NOT_AVAILABLE
#include "corNgsild/ldDistOp.h"                           // ldDistOpEndpointIsSelf

#include "corJsonld/CorLdContext.h"                       // CorLdContext
#include "corJsonld/CorLdContextCache.h"                  // CorLdContextCache
#include "corJsonld/corLdCache.h"                         // corLdCacheLookup

#include "db/DbDriver.h"                                  // db, DbContextRow, DB_OK

#include "startup/startupContext.h"                       // Own interface



extern CorLdContextCache* corLdCacheGet(void);            // corJsonld/corLdInit.c - the cache's allocator



// -----------------------------------------------------------------------------
//
// ownContextId - the id of a @context this broker hosts, from the URL it gives it - its own
// <scheme>://<host>:<port>, then /ngsi-ld/v1/jsonldContexts/<id> - percent-decoded into buf (a urn's ':'
// may come as %3A); NULL for any other URL
//
static const char* ownContextId(const char* url, char* buf, int bufSize)
{
  if (ldDistOpEndpointIsSelf(url) == false)
    return NULL;

  const char* pathP = strchr(strstr(url, "://") + 3, '/');

  if ((pathP == NULL) || (strncmp(pathP, "/ngsi-ld/v1/jsonldContexts/", 27) != 0) || (pathP[27] == 0))
    return NULL;

  const char* idP = &pathP[27];
  int         at  = 0;

  for (; (*idP != 0) && (*idP != '?') && (*idP != '#') && (at < bufSize - 1); idP++)
  {
    if ((idP[0] == '%') && (idP[1] != 0) && (idP[2] != 0))
    {
      char hex[3] = { idP[1], idP[2], 0 };
      buf[at++]   = (char) strtol(hex, NULL, 16);
      idP        += 2;
    }
    else
      buf[at++] = *idP;
  }
  buf[at] = 0;

  return buf;
}



// -----------------------------------------------------------------------------
//
// ownContextBody - a malloc'd copy of the body of a @context this broker hosts - from its cache, else its
// store, as GET /jsonldContexts/{id} reads it; NULL if it has none by that id
//
static char* ownContextBody(const char* id)
{
  CorLdContext* cachedP = corLdCacheLookup(id);
  const char*   body    = (cachedP != NULL) ? cachedP->body : NULL;
  DbContextRow  row;

  if ((body == NULL) && (db.contextGet != NULL) && (db.contextGet(id, corLdCacheGet()->kaP, &row) == DB_OK))
    body = row.body;

  if (body == NULL)
    return NULL;

  int   len  = strlen(body);
  char* copy = (char*) malloc(len + 1);

  if (copy != NULL)
    memcpy(copy, body, len + 1);

  return copy;
}



// -----------------------------------------------------------------------------
//
// startupContextDownload - the body of the @context at url. One this broker hosts, by the URL it gives it,
// is read from its own cache or store - no request to itself; any other is fetched
//
char* startupContextDownload(const char* url, int* statusCodeP)
{
  char        idBuf[512];
  const char* id = ownContextId(url, idBuf, sizeof(idBuf));

  if (id != NULL)
  {
    char* body = ownContextBody(id);

    if (body != NULL)
    {
      *statusCodeP = 200;
      return body;
    }
  }

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

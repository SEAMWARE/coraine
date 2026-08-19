//
// FILE            postExNotification.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/ex/v1/notifications/{parentSubId} — receive a
// notification produced by a remote Context Source for a derived
// subscription that this broker forwarded (NGSI-LD § 5.8.1.4).
//
// The handler:
//   * looks up parentSubId in the local subscription cache,
//   * rewrites the body's "subscriptionId" field back to parentSubId
//     (the remote knew the derived id, our subscriber expects the
//     parent's), and
//   * POSTs the body to the original sub's notification endpoint.
//
// Always replies 204 to the remote when the local sub exists — the
// fact that our subscriber accepted or refused the re-dispatch is
// not something the remote should care about. Missing parent → 404.
//

#include <string.h>                                  // strlen, strcmp, strncasecmp
#include <stdio.h>                                   // snprintf

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/corRestClient.h"                     // CorRestClientRequest, corRestClientSend
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjRender.h"                          // kjFastRender
#include "kjson/kjRenderSize.h"                      // kjFastRenderSize
#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "corJsonld/corLdInit.h"                       // corLdCoreContext
#include "corJsonld/corLdCompactTree.h"                // corLdCompactTree

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdSubCache.h"                     // LdSubCache, LdSubCacheItem
#include "corNgsild/ldSubCache.h"                     // ldSubCacheItemLookup
#include "corNgsild/ldNotifyStatsHook.h"              // ldNotifyStatsHookInvoke

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postExNotification.h"      // Own interface



// -----------------------------------------------------------------------------
//
// postExNotification -
//
bool postExNotification(void)
{
  const char* parentSubId = corRest.in.wildcard[0];

  if (parentSubId == NULL || parentSubId[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Missing URL Component", "missing parent subscription id in URL");
    return true;
  }

  Tenant* tenantP = (Tenant*) corNgsild.tenantP;
  if (tenantP->subCacheP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "no subscription cache for this tenant");
    return true;
  }

  LdSubCacheItem* itemP = ldSubCacheItemLookup((LdSubCache*) tenantP->subCacheP, parentSubId);
  if (itemP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "parent subscription '%s' not found", parentSubId);
    return true;
  }

  //
  // Body is parsed by the framework. parseHook left it in the same
  // expanded shape as any other application/json payload — the keys
  // we touch here ("subscriptionId", "data") are core-context terms
  // and survive expansion as short names.
  //
  KjNode* bodyTree = corRest.in.requestTree;
  if (bodyTree == NULL || bodyTree->type != KjObject)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Not a JSON Object",
            "notification body must be a JSON object");
    return true;
  }

  //
  // Rewrite subscriptionId from derived to parent. Notifications without
  // the field are still re-dispatched (best-effort).
  //
  KjNode* subIdP = kjLookup(bodyTree, "subscriptionId");
  if (subIdP != NULL && subIdP->type == KjString)
    subIdP->value.s = (char*) parentSubId;

  //
  // Drop the subscriber forward when the parent is paused / expired.
  // Returning 204 keeps the remote happy (no retry storm).
  //
  bool isActive = (itemP->status == LdSubStatusActive);
  if (!isActive || itemP->endpointUri == NULL)
  {
    corRest.out.httpStatusCode = 204;
    return true;
  }

  //
  // parseHook expanded the keys against the request's @context. Compact
  // them back so the subscriber sees short names — same shape any local
  // sub notification would carry. Core context handles default-vocab
  // stripping (Vehicle, speed, …) without needing the parent's user ctx.
  //
  corLdCompactTree(bodyTree);

  //
  // Render the (modified) body and POST to the original subscriber.
  //
  int   bodyLen = kjFastRenderSize(bodyTree) + 1;
  char* body    = (char*) kaAlloc(&corRest.kalloc, bodyLen);
  kjFastRender(bodyTree, body);

  CorRestClientRequest  req;
  CorRestClientResponse resp;

  corRestClientRequestInit(&req, CorVerbPost, itemP->endpointUri, NULL);
  corRestClientRequestHeader(&req, "Content-Type", "application/json");

  const char* ctxUrl = (itemP->contextUrl != NULL) ? itemP->contextUrl : NULL;
  if (ctxUrl == NULL)
  {
    CorLdContext* coreP = corLdCoreContext();
    if (coreP != NULL)
      ctxUrl = coreP->url;
  }
  if (ctxUrl != NULL)
  {
    char linkBuf[512];
    snprintf(linkBuf, sizeof(linkBuf),
             "<%s>; rel=\"http://www.w3.org/ns/json-ld#context\"; type=\"application/ld+json\"",
             ctxUrl);
    corRestClientRequestHeader(&req, "Link", linkBuf);
  }

  corRestClientRequestBody(&req, body, strlen(body));
  corRestClientRequestTimeout(&req, 5000, 10000);

  corRestClientSend(&req, &resp);
  corRestClientResponseCleanup(&resp);   // free the grown response header vector

  //
  // Update the parent sub's notification counters — same accounting as
  // a locally-emitted notification.
  //
  itemP->timesSent       += 1;
  itemP->lastNotification = corRest.requestStartTime;

  bool ok = (resp.statusCode >= 200 && resp.statusCode < 300);
  if (ok)
    itemP->lastSuccess = corRest.requestStartTime;
  else
  {
    itemP->timesFailed += 1;
    itemP->lastFailure  = corRest.requestStartTime;
  }

  ldNotifyStatsHookInvoke(false /*csrSub*/, ok);

  corRest.out.httpStatusCode = 204;
  return true;
}

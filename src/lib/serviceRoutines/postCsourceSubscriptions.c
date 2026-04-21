//
// FILE            postCsourceSubscriptions.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/csourceSubscriptions  (NGSI-LD § 5.11.2)
//
// Creates a CSR-subscription in the per-tenant regSubCacheP. Uses the
// same Subscription data type as entity-subs but is routed to a
// separate cache instance so the entity-notify hot path never iterates
// registration-subs (and vice versa).
//
// V1 scope: cache-only, no DB persistence. timeInterval -> 501.
// Initial-on-subscribe notification is emitted from here (§ 5.11.2.4 /
// § 5.11.7): a Context Source Notification with all currently
// matching CSRs and triggerReason="newlyMatching" is POSTed to the
// subscription's endpoint.
//
#include <string.h>                                  // strlen, strcpy, strcat
#include <stdio.h>                                   // snprintf
#include <time.h>                                    // time

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjBuilder.h"                         // kjString, kjChildAdd
#include "kjson/KjNode.h"                            // KjNode, KjString
#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "swJsonld/swldInit.h"                       // swldCoreContext
#include "swJsonld/SwldContext.h"                    // SwldContext
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldCheckSubscription.h"            // ldCheckSubscription
#include "swNgsild/LdOp.h"                           // LdOpCreateCsourceSubscription
#include "swNgsild/LdVocab.h"                        // LD_VOCAB_IS_ACTIVE, LD_VOCAB_STATUS
#include "swNgsild/LdSubCache.h"                     // LdSubCache
#include "swNgsild/ldSubCache.h"                     // ldSubCacheItemAdd, ldSubCacheItemLookup

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postCsourceSubscriptions.h"  // Own interface



static char* csrSubIdGenerate(KAlloc* allocP)
{
  static int counter = 0;
  char*      buf     = kaAlloc(allocP, 128);

  snprintf(buf, 128, "urn:ngsi-ld:Subscription:%lx:%04x", (long) time(NULL), ++counter & 0xFFFF);

  return buf;
}



bool postCsourceSubscriptions(void)
{
  if (swNgsild.contextError)
    return true;

  KjNode* subP = swRest.in.requestTree;

  if (swRest.in.payload != NULL && subP == NULL)
  {
    ldError(415, LD_ERROR_INVALID_REQUEST, "Unsupported Media Type",
            "supported Content-Types: application/json, application/ld+json");
    return true;
  }

  if (subP == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "no payload");
    return true;
  }

  if (ldCheckSubscription(subP, LdOpCreateCsourceSubscription, &swRest.kalloc) == false)
    return true;

  // timeInterval (periodic CSR notifications) deferred — § 5.11.7
  if (kjLookup(subP, "timeInterval") != NULL)
  {
    ldError(501, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
            "periodic CSR subscriptions ('timeInterval') are not supported");
    return true;
  }

  //
  // Extract or generate subscription id
  //
  KjNode* idP = kjLookup(subP, "id");

  if (idP == NULL)
  {
    char* generatedId = csrSubIdGenerate(&swRest.kalloc);

    idP = kjString(NULL, "id", generatedId);
    kjChildAdd(subP, idP);
  }
  else if (idP->type != KjString)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "subscription 'id' must be a string");
    return true;
  }

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  // Reject duplicate id in the CSR-sub cache
  if (tenantP->regSubCacheP != NULL
      && ldSubCacheItemLookup((LdSubCache*) tenantP->regSubCacheP, idP->value.s) != NULL)
  {
    ldError(409, LD_ERROR_ALREADY_EXISTS, "Already Exists",
            "CSR subscription '%s' already exists", idP->value.s);
    return true;
  }

  //
  // status = "active"|"paused" — expiresAt-past check
  //
  KjNode* isActiveP  = kjLookup(subP, LD_VOCAB_IS_ACTIVE);
  bool    isActive   = (isActiveP == NULL || isActiveP->type != KjBoolean || isActiveP->value.b == true);

  KjNode* statusP = kjString(NULL, LD_VOCAB_STATUS, isActive ? "active" : "paused");
  kjChildAdd(subP, statusP);

  //
  // jsonldContext (for notification compaction)
  //
  {
    SwldContext* reqCtxP = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();
    if (reqCtxP != NULL && reqCtxP->url != NULL)
    {
      KjNode* jcP = kjString(NULL, "jsonldContext", reqCtxP->url);
      kjChildAdd(subP, jcP);
    }
  }

  //
  // Add to CSR-subscription cache
  //
  if (tenantP->regSubCacheP != NULL)
    ldSubCacheItemAdd((LdSubCache*) tenantP->regSubCacheP, subP, NULL);

  //
  // 201 Created — Location + Link headers
  //
  swRest.out.httpStatusCode = 201;

  SwRestKeyValue* hV = swRest.out.headerV;
  int ix = swRest.out.headerCount;

  const char* prefix  = "/ngsi-ld/v1/csourceSubscriptions/";
  int         locLen  = strlen(prefix) + strlen(idP->value.s) + 1;
  char*       locBuf  = kaAlloc(&swRest.kalloc, locLen);

  strcpy(locBuf, prefix);
  strcat(locBuf, idP->value.s);
  hV[ix].key   = "Location";
  hV[ix].value = locBuf;
  ix++;

  SwldContext* ctxP   = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();
  const char*  ctxUrl = ctxP->url;

  if (ctxUrl != NULL)
  {
    const char* suffix  = ">; rel=\"http://www.w3.org/ns/json-ld#context\"; type=\"application/ld+json\"";
    int         linkLen = 1 + strlen(ctxUrl) + strlen(suffix) + 1;
    char*       linkBuf = kaAlloc(&swRest.kalloc, linkLen);

    strcpy(linkBuf, "<");
    strcat(linkBuf, ctxUrl);
    strcat(linkBuf, suffix);
    hV[ix].key   = "Link";
    hV[ix].value = linkBuf;
    ix++;
  }

  swRest.out.headerCount = ix;

  //
  // Initial-on-subscribe notification (§ 5.11.2.4 / § 5.11.7) will be
  // dispatched by the CSR-sub matcher/notifier — hooked after cache add.
  // Placeholder — see ldCsrSubNotifyInitial (added in follow-up commit).
  //

  return true;
}

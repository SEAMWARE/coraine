//
// FILE            bridgeEntitiesUpsert.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The entities a bridge's DataChannel or AssetChannel made -> one batch upsert. See bridgeEntitiesUpsert.h.
//
// The write goes through the service routine of POST /entityOperations/upsert, in this process, with no
// request around it - as coraine-import brings in subscriptions and registrations (migrateApiObject.c). An
// entity a bridge made is then checked, stored, notified and recorded in the temporal store exactly as one a
// client sends, and nothing here knows how.
//
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corLog/corLog.h"                            // COR_W

#include "corRest/corRest.h"                          // corRest
#include "corNgsild/CorNgsild.h"                      // corNgsild, ldDefaultContext
#include "corNgsild/ldHooks.h"                        // corNgsildFallbackRelease
#include "corNgsild/ldNotifyDefer.h"                  // ldNotifyDispatchPending
#include "corNgsild/ldCsrSubNotify.h"                 // ldCsrSubDispatchPending
#include "corNgsild/ldCheckSubscription.h"            // ldSubEntityTypeExprsRelease

#include "troe/troeDispatch.h"                        // troeDispatchPending
#include "serviceRoutines/postEntityBatchUpsert.h"    // postEntityBatchUpsert
#include "bridge/bridgeServiceSync.h"                 // bridgeRequestsReleasePending
#include "bridge/bridgeEntitiesUpsert.h"              // Own interface



// -----------------------------------------------------------------------------
//
// bridgeEntitiesUpsert -
//
int bridgeEntitiesUpsert(CorNode* entitiesP, int entities, const char* what)
{
  corRest.in.requestTree         = entitiesP;
  corRest.out.httpStatusCode     = 200;
  corRest.out.headerV            = corRest.out.headers;
  corRest.out.headerCount        = 0;
  corRest.out.headerSize         = sizeof(corRest.out.headers) / sizeof(corRest.out.headers[0]);
  corRest.out.responseTree       = NULL;
  corRest.out.problemType        = NULL;
  corRest.out.problemTitle       = NULL;
  corRest.out.problemDetail[0]   = 0;

  corNgsild.contextP             = ldDefaultContext(&corRest.kalloc);
  corNgsild.userContextBody      = NULL;
  corNgsild.batchPreErrors       = NULL;
  corNgsild.local                = true;              // this broker's store: what a bridge made is not forwarded
  corNgsild.upsertUpdate         = true;              // options=update: the attributes the mapping does not name stay
  corNgsild.bridgeOwnWrite       = true;              // the bridge's own write: its payload check is for clients

  postEntityBatchUpsert();

  corNgsild.local                = false;
  corNgsild.upsertUpdate         = false;
  corNgsild.bridgeOwnWrite       = false;

  //
  // What the request's post-response hook does after a write
  //
  ldNotifyDispatchPending();
  ldCsrSubDispatchPending();
  troeDispatchPending();
  bridgeRequestsReleasePending();
  ldSubEntityTypeExprsRelease();
  corNgsildFallbackRelease();

  int status = corRest.out.httpStatusCode;

  if ((status == 201) || (status == 204))
    return entities;

  if (status != 207)
  {
    COR_W("%s: no entity written (%d: %s)", what, status, corRest.out.problemDetail);
    return 0;
  }

  CorNode* errorsP  = (corRest.out.responseTree != NULL) ? corTreeLookup(corRest.out.responseTree, "errors")  : NULL;
  CorNode* successP = (corRest.out.responseTree != NULL) ? corTreeLookup(corRest.out.responseTree, "success") : NULL;
  int      written  = 0;

  for (CorNode* nodeP = (successP != NULL) ? successP->value.head : NULL; nodeP != NULL; nodeP = nodeP->next)
    written++;

  for (CorNode* errP = (errorsP != NULL) ? errorsP->value.head : NULL; errP != NULL; errP = errP->next)
  {
    CorNode* idP     = corTreeLookup(errP, "entityId");
    CorNode* pdP     = corTreeLookup(errP, "error");
    CorNode* detailP = (pdP != NULL) ? corTreeLookup(pdP, "detail") : NULL;

    COR_W("%s: entity '%s' not written: %s", what,
          ((idP     != NULL) && (idP->type     == CorString)) ? idP->value.s     : "?",
          ((detailP != NULL) && (detailP->type == CorString)) ? detailP->value.s : "?");
  }

  return written;
}

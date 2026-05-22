//
// FILE            deleteEntity.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strlen, strcpy
#include <stdlib.h>                                  // free
#include <stdio.h>                                   // snprintf

#include "swRest/SwRestState.h"                      // swRest
#include "swRest/SwRestVerb.h"                       // SwVerbDelete

#include "kjson/kjBuilder.h"                         // kjObject, kjArray, kjString, kjChildAdd
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjLookup.h"                          // kjLookup

#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdProblem.h"                      // LD_ERROR_CONFLICT
#include "swNgsild/LdSubCache.h"                     // LdSubCache
#include "swNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityDelete
#include "swNgsild/ldNotifyDefer.h"                  // ldNotifyDefer

#include "troe/TroeDriver.h"                         // TroeEvent, TroeOpEntityDeleted
#include "troe/troeDispatch.h"                       // troeDeferEntityEvent

#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "swNgsild/ldDistOp.h"                       // ldDistOpLoopDetected, ldDistOpSend, ldDistOpBatchErrorAdd

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/deleteEntity.h"            // Own interface



// -----------------------------------------------------------------------------
//
// deleteUrl - compose the DELETE forward URL for one CSR
//
// <endpoint>/ngsi-ld/v1/entities/<entityId>
//
static char* deleteUrl(const char* endpoint, const char* entityId)
{
  const char* path    = "/ngsi-ld/v1/entities/";
  int         baseLen = strlen(endpoint);
  int         pathLen = strlen(path);
  int         idLen   = strlen(entityId);
  char*       url     = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + idLen + 1);
  strcpy(url, endpoint);
  strcpy(url + baseLen, path);
  strcpy(url + baseLen + pathLen, entityId);
  return url;
}



// -----------------------------------------------------------------------------
//
// deleteEntity -
//
bool deleteEntity(void)
{
  const char* entityId = swRest.in.wildcard[0];

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  //
  // Dispatch — § 5.6.6.4. Skipped when:
  //   - ?local=true;
  //   - tenant has no registration cache (no CSRs could match anyway);
  //   - the incoming request already carries our own Via alias (loop
  //     detected — skip forwards but STILL run local delete, since
  //     preventing the forward is the only thing loop-detection
  //     demands; the request itself is legitimate).
  //
  KjNode* errorsArrayP = kjArray(swRest.kjsonP, "errors");
  bool    anySucceeded = false;

  bool dispatch = (swNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;   // forwards suppressed; local still runs below.

  if (dispatch)
  {
    char** typeArr = swNgsild.typeV;

    LdRegCacheItem** exclV  = NULL;
    LdRegCacheItem** redirV = NULL;
    LdRegCacheItem** inclV  = NULL;
    int exclN  = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  entityId, typeArr, NULL,
                                                  LdRegModeExclusive, &exclV);
    int redirN = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  entityId, typeArr, NULL,
                                                  LdRegModeRedirect, &redirV);
    int inclN  = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  entityId, typeArr, NULL,
                                                  LdRegModeInclusive, &inclV);

    LdDistOpGroup groups[] = {
      { exclV,  exclN,  "exclusive", true  },
      { redirV, redirN, "redirect",  true  },
      { inclV,  inclN,  "inclusive", false },
    };

    LdDistOpEntry* items;
    int n = ldDistOpEntriesBuild(groups, 3, ownAlias,
                                  swRest.serviceP->ldOp, "deleteEntity",
                                  entityId, /*perRi=*/false, NULL, NULL,
                                  errorsArrayP, &items);

    for (int i = 0; i < n; i++)
      items[i].url = deleteUrl(items[i].csr->endpoint, entityId);

    ldDistOpEntriesPerform(items, n, SwVerbDelete, ownAlias);

    for (int i = 0; i < n; i++)
    {
      int sc = items[i].statusCode;
      if (sc >= 200 && sc < 300)
        anySucceeded = true;
      else if (sc != 404)
        ldDistOpBatchErrorAdd(errorsArrayP, entityId, (sc >= 400) ? sc : 502,
                              LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                              ldDistOpForwardFailureReason(sc, items[i].errorDetail),
                              items[i].csr->regId);
    }

    if (exclV  != NULL) free(exclV);
    if (redirV != NULL) free(redirV);
    if (inclV  != NULL) free(inclV);
  }

  //
  // Local delete — always attempted (§ 5.6.6.4 "used to remove the
  // entity locally if it exists"). Retrieve pre-image first if subs are
  // active so notifications have an entity body.
  //
  KjNode* entityP = NULL;
  if (tenantP->subCacheP != NULL)
    db.entityRetrieve(tenantP, entityId, &entityP);

  int r = db.entityDelete(tenantP, entityId);

  if (r == DB_OK)
  {
    anySucceeded = true;

    if (tenantP->subCacheP != NULL && entityP != NULL)
      ldNotifyDeferDelete((LdSubCache*) tenantP->subCacheP, entityP, swRest.requestStartTime);

    // TRoE: entity-level tombstone. Attribute timelines are still
    // queryable; the temporal-query reader joins this row to scope
    // alive windows.
    {
      const char* etype = NULL;
      if (entityP != NULL)
      {
        KjNode* tn = kjLookup(entityP, "type");
        if (tn != NULL && tn->type == KjString) etype = tn->value.s;
      }
      TroeEvent* tevP = (TroeEvent*) kaAlloc(&swRest.kalloc, sizeof(TroeEvent));
      memset(tevP, 0, sizeof(*tevP));
      tevP->op             = TroeOpEntityDeleted;
      tevP->tenantP        = tenantP;
      tevP->entityId       = entityId;
      tevP->entityType     = etype;
      tevP->modifiedAtNs   = swRest.requestStartTime;
      tevP->entitySnapshot = entityP;     // pre-delete snapshot, NULL-safe
      troeDeferEntityEvent(tevP);
    }
  }
  else if (r != DB_NOT_FOUND)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "database error deleting entity '%s'", entityId);
    return true;
  }
  // r == DB_NOT_FOUND tolerated when a CSR carried the delete.

  //
  // Response decision:
  //   - nothing succeeded AND no errors → 404 Not Found
  //   - nothing succeeded AND errors[] non-empty → 409 Conflict + body
  //   - something succeeded AND errors[] empty → 204 No Content
  //   - something succeeded AND errors[] non-empty → 207 Multi-Status + body
  //
  int errorsCount = 0;
  for (KjNode* p = errorsArrayP->value.firstChildP; p != NULL; p = p->next) errorsCount++;

  if (!anySucceeded && errorsCount == 0)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity '%s' not found", entityId);
    return true;
  }

  if (errorsCount == 0)
  {
    swRest.out.httpStatusCode = 204;
    return true;
  }

  KjNode* successArrayP = kjArray(swRest.kjsonP, "success");
  if (anySucceeded)
    kjChildAdd(successArrayP, kjString(swRest.kjsonP, NULL, entityId));

  KjNode* respBodyP = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(respBodyP, successArrayP);
  kjChildAdd(respBodyP, errorsArrayP);

  swRest.out.responseTree   = respBodyP;
  swRest.out.httpStatusCode = anySucceeded ? 207 : 409;

  return true;
}

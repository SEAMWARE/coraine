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

#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdProblem.h"                      // LD_ERROR_CONFLICT
#include "swNgsild/LdSubCache.h"                     // LdSubCache
#include "swNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityDelete
#include "swNgsild/ldNotifyDefer.h"                  // ldNotifyDefer

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
                   && tenantP != NULL
                   && tenantP->regCacheP != NULL);

  const char* ownAlias = (tenantP != NULL)
                         ? ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc)
                         : NULL;

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

    //
    // All three modes share the same per-CSR loop body for deleteEntity:
    // check op support, forward, classify. Inclusive differs only in that
    // op-not-supported is silently skipped (spec § 5.6.6.4: forwarding is
    // conditional on "Delete Entity operation is supported").
    //
    LdRegCacheItem** groups[]       = { exclV,     redirV,    inclV     };
    int              counts[]       = { exclN,     redirN,    inclN     };
    const char*      modeTag[]      = { "exclusive", "redirect", "inclusive" };
    bool             opConflict[]   = { true,      true,      false     };

    for (int g = 0; g < 3; g++)
    {
      for (int i = 0; i < counts[g]; i++)
      {
        LdRegCacheItem* csr = groups[g][i];
        if (csr->endpoint == NULL)
          continue;

        // Proactive loop-detect (§ 5.12): CSR alias known + in chain → skip
        if (ldDistOpCsrWouldLoop(csr, ownAlias))
          continue;

        if (!ldRegOpSupported(csr, swRest.serviceP->ldOp))
        {
          if (!opConflict[g])
            continue;  // inclusive: silently skip

          char detail[256];
          snprintf(detail, sizeof(detail),
                   "%s registration does not support deleteEntity", modeTag[g]);
          ldDistOpBatchErrorAdd(errorsArrayP, entityId,
                                LD_ERROR_CONFLICT, "Conflict", detail, csr->regId);
          continue;
        }

        const char* upErr  = NULL;
        int         upCode = ldDistOpSend(csr, SwVerbDelete,
                                          deleteUrl(csr->endpoint, entityId),
                                          NULL, 0, ownAlias, &upErr);

        // 2xx = remote confirmed the delete.
        // 404 = remote never had it; not a success, not an error either
        //       (consistent with local DB_NOT_FOUND being tolerated) —
        //       skip silently so the overall-404 path still reports
        //       "entity not found anywhere".
        // Anything else → BatchEntityError.
        if (upCode >= 200 && upCode < 300)
          anySucceeded = true;
        else if (upCode != 404)
          ldDistOpBatchErrorAdd(errorsArrayP, entityId,
                                LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                                ldDistOpForwardFailureReason(upCode, upErr), csr->regId);
      }
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
  if (tenantP != NULL && tenantP->subCacheP != NULL)
    db.entityRetrieve(tenantP, entityId, &entityP);

  int r = db.entityDelete(tenantP, entityId);

  if (r == DB_OK)
  {
    anySucceeded = true;

    if (tenantP != NULL && tenantP->subCacheP != NULL && entityP != NULL)
      ldNotifyDeferDelete((LdSubCache*) tenantP->subCacheP, entityP, swRest.requestStartTime);
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

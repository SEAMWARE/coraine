//
// FILE            replaceEntity.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// PUT /ngsi-ld/v1/entities/{entityId} — Replace Entity.
// NGSI-LD v1.9.1 § 5.6.18 / § 5.5.12.
//

#include <stddef.h>                                   // NULL
#include <string.h>                                   // strcmp, strlen, strcpy
#include <stdlib.h>                                   // free
#include <stdio.h>                                    // snprintf

#include "swRest/SwRestState.h"                       // swRest
#include "swRest/SwRestVerb.h"                        // SwVerbPut

#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/kjBuilder.h"                          // kjObject, kjArray, kjString, kjChildAdd
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjRender.h"                           // kjFastRender
#include "kjson/kjRenderSize.h"                       // kjFastRenderSize

#include "kalloc/kaAlloc.h"                           // kaAlloc

#include "swJsonld/swldInit.h"                        // swldCoreContext, SWLD_CORE_CONTEXT_URL

#include "swNgsild/swNgsild.h"                        // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldCheckEntity.h"                   // ldCheckEntity
#include "swNgsild/LdOp.h"                            // LdOpReplaceEntity
#include "swNgsild/ldApiEntityToDbModel.h"            // ldApiEntityToDbModel
#include "swNgsild/LdProblem.h"                       // LD_ERROR_CONFLICT
#include "swNgsild/LdSubCache.h"                      // LdSubCache
#include "swNgsild/ldSubscriptionNotify.h"            // LdNotifyEntityUpdate
#include "swNgsild/ldNotifyDefer.h"                   // ldNotifyDefer

#include "swNgsild/LdRegCache.h"                      // LdRegCache, LdRegCacheItem, LdRegMode, LdRegInfo
#include "swNgsild/ldRegCache.h"                      // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "swNgsild/ldCsourceAlias.h"                  // ldCsourceAliasForTenant
#include "swNgsild/ldDistOp.h"                        // ldDistOpLoopDetected, ldDistOpSend, ldDistOpBatchErrorAdd
#include "swNgsild/ldEntityFragment.h"                // ldEntityFragmentForInfo

#include "db/DbDriver.h"                              // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                                // Tenant

#include "serviceRoutines/replaceEntity.h"            // Own interface



// -----------------------------------------------------------------------------
//
// typeEqual - compare two NGSI-LD entity "type" nodes for equality.
//
// Both must be the same shape (string vs. array) and carry the same set of
// values. String comparison is exact (the values are expanded IRIs).
//
static bool typeEqual(KjNode* a, KjNode* b)
{
  if ((a == NULL) || (b == NULL))
    return false;

  if ((a->type == KjString) && (b->type == KjString))
    return strcmp(a->value.s, b->value.s) == 0;

  if ((a->type != KjArray) || (b->type != KjArray))
    return false;

  for (KjNode* aI = a->value.firstChildP; aI != NULL; aI = aI->next)
  {
    if (aI->type != KjString)
      return false;

    bool found = false;
    for (KjNode* bI = b->value.firstChildP; bI != NULL; bI = bI->next)
    {
      if ((bI->type == KjString) && (strcmp(aI->value.s, bI->value.s) == 0))
      {
        found = true;
        break;
      }
    }
    if (!found)
      return false;
  }

  for (KjNode* bI = b->value.firstChildP; bI != NULL; bI = bI->next)
  {
    if (bI->type != KjString)
      return false;

    bool found = false;
    for (KjNode* aI = a->value.firstChildP; aI != NULL; aI = aI->next)
    {
      if ((aI->type == KjString) && (strcmp(bI->value.s, aI->value.s) == 0))
      {
        found = true;
        break;
      }
    }
    if (!found)
      return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// entityInfoCoversId - does any EntityInfo entry in riP cover entityId?
//
static bool entityInfoCoversId(LdRegInfo* riP, const char* entityId)
{
  for (LdRegEntityInfo* eiP = riP->entityInfoV; eiP != NULL; eiP = eiP->next)
  {
    if (eiP->id == NULL && eiP->idPatternList == NULL)
      return true;
    if (eiP->id != NULL && strcmp(eiP->id, entityId) == 0)
      return true;
    for (LdRegIdPattern* patP = eiP->idPatternList; patP != NULL; patP = patP->next)
      if (regexec(&patP->regex, entityId, 0, NULL, 0) == 0)
        return true;
  }
  return false;
}



// -----------------------------------------------------------------------------
//
// replaceUrl - compose the PUT forward URL for one CSR
//
// <endpoint>/ngsi-ld/v1/entities/<entityId>
//
static char* replaceUrl(const char* endpoint, const char* entityId)
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
// renderFragmentWithContext - serialize fragment with @context for remote
//
static char* renderFragmentWithContext(KjNode* fragP)
{
  if (kjLookup(fragP, "@context") == NULL)
  {
    KjNode* ctxNode = kjString(swRest.kjsonP, "@context", SWLD_CORE_CONTEXT_URL);
    kjChildAdd(fragP, ctxNode);
  }

  int   bufSize = kjFastRenderSize(fragP) + 1;
  char* buf     = (char*) kaAlloc(&swRest.kalloc, bufSize);

  kjFastRender(fragP, buf);
  return buf;
}



// -----------------------------------------------------------------------------
//
// forwardReplaceEntity - PUT entity fragment to a CSR endpoint
//
static int forwardReplaceEntity(LdRegCacheItem* csr,
                                KjNode*         fragP,
                                const char*     entityId,
                                const char*     ownAlias,
                                const char**    errorDetailPP)
{
  char* body = renderFragmentWithContext(fragP);
  return ldDistOpSend(csr, SwVerbPut,
                      replaceUrl(csr->endpoint, entityId),
                      body, strlen(body), ownAlias, errorDetailPP);
}



// -----------------------------------------------------------------------------
//
// replaceEntity -
//
bool replaceEntity(void)
{
  //
  // @context error detected in parseHook
  //
  if (swNgsild.contextError)
    return true;

  const char* entityId = swRest.in.wildcard[0];
  KjNode*     entityP  = swRest.in.requestTree;

  //
  // Unsupported Content-Type (payload present but not parsed as JSON)
  //
  if (swRest.in.payload != NULL && entityP == NULL)
  {
    ldError(415, LD_ERROR_INVALID_REQUEST, "Unsupported Media Type",
            "supported Content-Types: application/json, application/ld+json");
    return true;
  }

  if (entityP == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "no payload");
    return true;
  }

  //
  // Validate payload as a full entity (id + type mandatory, no null-marker).
  //
  if (ldCheckEntity(entityP, LdOpReplaceEntity, NULL, &swRest.kalloc) == false)
    return true;

  //
  // Id consistency: body id (if present) must match URL id
  //
  KjNode* bodyIdP = kjLookup(entityP, "id");
  if (bodyIdP != NULL && bodyIdP->type == KjString && strcmp(bodyIdP->value.s, entityId) != 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "entity id in payload ('%s') does not match URL ('%s')",
            bodyIdP->value.s, entityId);
    return true;
  }

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  //
  // Pre-retrieve local entity — used both to enforce the "type shall not
  // change" guard (§ 5.6.18 local procedure) and to detect whether the
  // local path will run at all (if local entity doesn't exist, we skip
  // the local replace and rely on forwards succeeding).
  //
  KjNode* oldStored   = NULL;
  int     rr          = db.entityRetrieve(tenantP, entityId, &oldStored);
  bool    localExists = (rr == DB_OK && oldStored != NULL);

  if (rr != DB_OK && rr != DB_NOT_FOUND)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "database error retrieving entity '%s'", entityId);
    return true;
  }

  //
  // Type-change guard: if a local entity exists, its stored type must match
  // the body's type (Replace Entity must not alter type — swBroker policy,
  // stricter than spec's local "completely replace"). Decide this BEFORE
  // any forward: a partial success where local refuses for type reasons
  // would be hard to express in a BatchOperationResult.
  //
  if (localExists)
  {
    KjNode* newTypeP = kjLookup(entityP, "type");
    KjNode* oldTypeP = kjLookup(oldStored, "type");

    if (!typeEqual(newTypeP, oldTypeP))
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "entity type cannot be changed on Replace");
      return true;
    }
  }

  //
  // Dispatch — § 5.6.18.4 Replace Entity. Skipped when:
  //   - ?local=true;
  //   - no registration cache on tenant;
  //   - incoming Via already carries our own alias (loop — skip forwards,
  //     still run local replace).
  //
  // Per § 5.6.18.4: "Attributes from matching input data are forwarded"
  // for each CSR. ldEntityFragmentForInfo extracts only the attrs from
  // entityP that the CSR's RegistrationInfo claims — id/type/@context
  // are cloned along so the forwarded body is a valid Replace Entity
  // input. Exclusive/redirect DETACH (chop); inclusive CLONE (keep
  // local copy for the post-dispatch remains).
  //
  KjNode* errorsArrayP = kjArray(swRest.kjsonP, "errors");
  bool    anySucceeded = false;

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

  bool dispatch = (swNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;

  if (dispatch)
  {
    KjNode* typeP = kjLookup(entityP, "type");
    char*   typeArr[2] = { NULL, NULL };
    char**  typeArgP   = NULL;
    if (typeP != NULL && typeP->type == KjString)
    {
      typeArr[0] = typeP->value.s;
      typeArgP   = typeArr;
    }

    LdRegCacheItem** exclV  = NULL;
    LdRegCacheItem** redirV = NULL;
    LdRegCacheItem** inclV  = NULL;
    int exclN  = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  entityId, typeArgP, NULL,
                                                  LdRegModeExclusive, &exclV);
    int redirN = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  entityId, typeArgP, NULL,
                                                  LdRegModeRedirect, &redirV);
    int inclN  = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  entityId, typeArgP, NULL,
                                                  LdRegModeInclusive, &inclV);

    //
    // Shared per-CSR loop across modes.
    //   - exclusive + redirect: DETACH matching attrs (they must not
    //     be applied locally — § 4.3.6.3). op-not-supported → Conflict.
    //   - inclusive: CLONE matching attrs (local keeps them too).
    //     op-not-supported → silent skip.
    //
    LdRegCacheItem** groups[]  = { exclV,       redirV,     inclV      };
    int              counts[]  = { exclN,       redirN,     inclN      };
    const char*      modeTag[] = { "exclusive", "redirect", "inclusive" };
    bool             detach[]  = { true,        true,       false      };
    bool             opConf[]  = { true,        true,       false      };

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

        bool opSupported = ldRegOpSupported(csr, swRest.serviceP->ldOp);

        for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
        {
          if (!entityInfoCoversId(riP, entityId))
            continue;

          KjNode* fragP = ldEntityFragmentForInfo(entityP, riP, swRest.kjsonP, detach[g]);
          if (fragP == NULL)
            continue;  // nothing in body matches this CSR's claim — skip

          if (!opSupported)
          {
            if (!opConf[g])
              continue;

            char detail[256];
            snprintf(detail, sizeof(detail),
                     "%s registration does not support replaceEntity", modeTag[g]);
            ldDistOpBatchErrorAdd(errorsArrayP, entityId,
                                  LD_ERROR_CONFLICT, "Conflict", detail, csr->regId);
            continue;
          }

          const char* upErr  = NULL;
          int         upCode = forwardReplaceEntity(csr, fragP, entityId, ownAlias, &upErr);

          // 2xx = remote applied the replace.
          // 404 = remote never had the entity; not a success, not an
          //       error either — consistent with delete/merge handling.
          // Anything else → BatchEntityError.
          if (upCode >= 200 && upCode < 300)
            anySucceeded = true;
          else if (upCode != 404)
            ldDistOpBatchErrorAdd(errorsArrayP, entityId,
                                  LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                                  ldDistOpForwardFailureReason(upCode, upErr), csr->regId);
        }
      }
    }

    if (exclV  != NULL) free(exclV);
    if (redirV != NULL) free(redirV);
    if (inclV  != NULL) free(inclV);
  }

  //
  // Local replace — § 5.6.18.4: "If the target Entity exists locally,
  // completely replace the existing Entity with the same Entity ID with
  // the new Entity content provided." "New Entity content" here means
  // the body AFTER exclusive/redirect chopping — so the local entity
  // ends up with only the unclaimed attrs.
  //
  // When local doesn't exist but forwards succeeded, we silently skip —
  // the entity is served by the CSRs, not locally. The response
  // decision below still reports 204/207 based on anySucceeded.
  //
  if (localExists && db.entityReplace == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented",
            "Replace Entity not supported by this DB plugin");
    return true;
  }

  if (localExists)
  {
    ldApiEntityToDbModel(entityP, &swRest.kalloc);

    KjNode* replacedOld = NULL;
    int     r           = db.entityReplace(tenantP, entityId, entityP, &replacedOld);

    if (r == DB_OK)
    {
      anySucceeded = true;

      // mongoc's entityReplace renames "id" to "_id" in-place. Restore.
      if (bodyIdP != NULL && bodyIdP->name[0] == '_')
        bodyIdP->name = "id";

      if (tenantP->subCacheP != NULL)
        ldNotifyDefer((LdSubCache*) tenantP->subCacheP, entityP, LdNotifyEntityUpdate, NULL);
    }
    else if (r != DB_NOT_FOUND)
    {
      ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
              "database error replacing entity '%s'", entityId);
      return true;
    }
    // DB_NOT_FOUND (race with concurrent delete) tolerated when forwards landed.
  }

  //
  // Response decision matrix:
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

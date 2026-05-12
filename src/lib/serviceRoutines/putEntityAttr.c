//
// FILE            putEntityAttr.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// PUT /ngsi-ld/v1/entities/{entityId}/attrs/{attrId}
//
// § 5.6.19 Replace Attribute. Replaces a single Attribute instance with
// the provided Attribute content. Fragment body cannot carry
// "urn:ngsi-ld:null" markers (Replace must yield a well-formed instance).
// Target instance is the default ("@none") unless datasetId in body
// selects another.
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp, strlen, memcpy
#include <stdlib.h>                                  // free
#include <stdio.h>                                   // snprintf

#include "swRest/SwRestState.h"                      // swRest
#include "swRest/SwRestVerb.h"                       // SwVerbPut

#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjChildAdd, kjChildRemove
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjRender.h"                          // kjFastRender
#include "kjson/kjRenderSize.h"                      // kjFastRenderSize

#include "swJsonld/swldInit.h"                       // SWLD_CORE_CONTEXT_URL
#include "swJsonld/swldExpand.h"                     // swldExpand

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdOp.h"                           // LdOpAppendAttrs
#include "swNgsild/ldCheckEntity.h"                  // ldCheckEntity
#include "swNgsild/ldNameContentCheck.h"             // ldIsValidName
#include "swNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "swNgsild/ldEntityMerge.h"                  // LdMergeReport
#include "swNgsild/LdVocab.h"                        // LD_VOCAB_SCOPE
#include "swNgsild/LdSubCache.h"                     // LdSubCache
#include "swNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityUpdate
#include "swNgsild/ldNotifyDefer.h"                  // ldNotifyDefer

#include "troe/TroeDriver.h"                         // TroeEvent, TroeOpAttrReplaced
#include "troe/troeDispatch.h"                       // troeDeferAttrEvent

#include "swNgsild/LdRegCache.h"                     // LdRegCache*, LdRegCacheItem, LdRegMode, LdRegInfo
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "swNgsild/ldDistOp.h"                       // ldDistOp*

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/putEntityAttr.h"           // Own interface



static char* attrUrl(const char* endpoint, const char* entityId, const char* attrWild)
{
  const char* p1 = "/ngsi-ld/v1/entities/";
  const char* p2 = "/attrs/";
  int  lenE = strlen(endpoint), lenP1 = strlen(p1), lenId = strlen(entityId);
  int  lenP2 = strlen(p2),      lenA  = strlen(attrWild);
  char* buf = (char*) kaAlloc(&swRest.kalloc, lenE + lenP1 + lenId + lenP2 + lenA + 1);
  char* p = buf;
  memcpy(p, endpoint, lenE); p += lenE;
  memcpy(p, p1, lenP1);      p += lenP1;
  memcpy(p, entityId, lenId); p += lenId;
  memcpy(p, p2, lenP2);      p += lenP2;
  memcpy(p, attrWild, lenA); p += lenA;
  *p = 0;
  return buf;
}



static char* renderBodyWithContext(KjNode* bodyP)
{
  if (kjLookup(bodyP, "@context") == NULL)
    kjChildAdd(bodyP, kjString(swRest.kjsonP, "@context", SWLD_CORE_CONTEXT_URL));
  int   bufSize = kjFastRenderSize(bodyP) + 1;
  char* buf     = (char*) kaAlloc(&swRest.kalloc, bufSize);
  kjFastRender(bodyP, buf);
  return buf;
}



// -----------------------------------------------------------------------------
//
// putEntityAttr -
//
bool putEntityAttr(void)
{
  const char* entityId = swRest.in.wildcard[0];
  const char* attrWild = swRest.in.wildcard[1];
  KjNode*     bodyP    = swRest.in.requestTree;

  if (bodyP->type != KjObject)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "attribute fragment must be a JSON object");
    return true;
  }

  // § 4.6.2 — the {attrId} URL path component must be a syntactically valid
  // NGSI-LD name (URN-style colon-separated segments, no leading '@', no
  // forbidden chars). ldCheckNamesAndContent runs only over body trees and
  // doesn't see the path component, so validate here. ETSI 055_03_03 sends
  // attrId="@invalid" and expects 400 BadRequestData; without this check
  // the broker would expand the bogus name via @vocab and 404 on lookup.
  if (!ldIsValidName(attrWild))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Invalid attribute name '%s' in URL path", attrWild);
    return true;
  }

  SwldContext* ctxP    = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();
  const char*  attrIri = swldExpand(ctxP, attrWild, &swRest.kalloc, NULL, NULL);
  if (attrIri == NULL) attrIri = attrWild;

  //
  // § 5.6.19.4: "If the target Attribute is scope, then an error of type
  // BadRequestData shall be raised."
  //
  if (strcmp(attrWild, "scope") == 0 || strcmp(attrIri, LD_VOCAB_SCOPE) == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "scope cannot be replaced via Replace Attribute");
    return true;
  }

  KjNode* ctxNodeP = kjLookup(bodyP, "@context");
  if (ctxNodeP != NULL) kjChildRemove(bodyP, ctxNodeP);

  //
  // Wrap the attribute fragment into a fake entity fragment.
  //
  KjNode* entityFrag = kjObject(swRest.kjsonP, NULL);
  bodyP->name = (char*) attrIri;
  kjChildAdd(entityFrag, bodyP);

  if (ldCheckEntity(entityFrag, LdOpAppendAttrs, NULL, &swRest.kalloc) == false)
    return true;

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  KjNode* errorsArrayP = kjArray(swRest.kjsonP, "errors");
  bool    anySucceeded = false;

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

  bool dispatch = (swNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;

  bool localApply = true;

  if (dispatch)
  {
    LdRegCacheItem** exclV  = NULL;
    LdRegCacheItem** redirV = NULL;
    LdRegCacheItem** inclV  = NULL;
    int exclN  = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  entityId, NULL, NULL,
                                                  LdRegModeExclusive, &exclV);
    int redirN = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  entityId, NULL, NULL,
                                                  LdRegModeRedirect, &redirV);
    int inclN  = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  entityId, NULL, NULL,
                                                  LdRegModeInclusive, &inclV);

    LdDistOpGroup groups[] = {
      { exclV,  exclN,  "exclusive", true  },
      { redirV, redirN, "redirect",  true  },
      { inclV,  inclN,  "inclusive", false },
    };
    static const bool detach[] = { true, true, false };

    LdDistOpEntry* items;
    int n = ldDistOpEntriesBuild(groups, 3, ownAlias,
                                  swRest.serviceP->ldOp, "replaceAttrs",
                                  entityId, /*perRi=*/true, entityId, attrIri,
                                  errorsArrayP, &items);

    for (int i = 0; i < n; i++)
    {
      KjNode* fwdBody = kjObject(swRest.kjsonP, NULL);
      for (KjNode* c = bodyP->value.firstChildP; c != NULL; c = c->next)
        kjChildAdd(fwdBody, c);

      char* bodyStr = renderBodyWithContext(fwdBody);
      items[i].url     = attrUrl(items[i].csr->endpoint, entityId, attrWild);
      items[i].body    = bodyStr;
      items[i].bodyLen = strlen(bodyStr);
    }

    ldDistOpEntriesPerform(items, n, SwVerbPut, ownAlias);

    for (int i = 0; i < n; i++)
    {
      int sc = items[i].statusCode;
      if (sc >= 200 && sc < 300)
      {
        anySucceeded = true;
        if (detach[items[i].modeIdx]) localApply = false;
      }
      else if (sc != 404)
        ldDistOpBatchErrorAdd(errorsArrayP, entityId,
                              LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                              ldDistOpForwardFailureReason(sc, items[i].errorDetail),
                              items[i].csr->regId);
    }

    if (exclV  != NULL) free(exclV);
    if (redirV != NULL) free(redirV);
    if (inclV  != NULL) free(inclV);
  }

  //
  // Local — retrieve first to verify the attribute exists, then replace
  // the matching instance via entityAttrsSet.
  //
  if (localApply)
  {
    KjNode* targetEntity = NULL;
    int     rr           = db.entityRetrieve(tenantP, entityId, &targetEntity);

    if (rr == DB_NOT_FOUND)
    {
      if (!anySucceeded)
      {
        ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                "entity '%s' not found", entityId);
        return true;
      }
    }
    else if (rr != DB_OK)
    {
      ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
              "database error retrieving entity '%s'", entityId);
      return true;
    }
    else
    {
      KjNode* tAttrP = kjLookup(targetEntity, attrIri);
      if (tAttrP == NULL)
      {
        if (!anySucceeded)
        {
          ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                  "attribute '%s' not found in entity '%s'", attrWild, entityId);
          return true;
        }
      }
      else
      {
        ldApiEntityToDbModel(entityFrag, &swRest.kalloc);

        if (db.entityAttrsSet == NULL)
        {
          ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
                  "Replace Attribute not supported by this DB plugin");
          return true;
        }

        LdMergeReport report = { NULL };
        int r = db.entityAttrsSet(tenantP, entityId, entityFrag, true,
                                   swRest.requestStartTime, &report);

        if (r != DB_OK && r != DB_NOT_FOUND)
        {
          ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
                  "database error replacing attribute '%s' on entity '%s'",
                  attrWild, entityId);
          return true;
        }

        if (r == DB_OK)
        {
          anySucceeded = true;

          KjNode* merged = NULL;
          if (tenantP->subCacheP != NULL)
            db.entityRetrieve(tenantP, entityId, &merged);

          if (tenantP->subCacheP != NULL && merged != NULL)
            ldNotifyDefer((LdSubCache*) tenantP->subCacheP, merged,
                          LdNotifyEntityUpdate, &report);

          // TRoE: defer one attrReplaced event. PUT semantics are
          // "wholesale replace this one attribute" — distinct from
          // PATCH's surgical modify.
          {
            const char* etype = NULL;
            if (merged != NULL)
            {
              KjNode* tn = kjLookup(merged, "type");
              if (tn != NULL && tn->type == KjString) etype = tn->value.s;
            }
            TroeEvent* tevP = (TroeEvent*) kaAlloc(&swRest.kalloc, sizeof(TroeEvent));
            memset(tevP, 0, sizeof(*tevP));
            tevP->op             = TroeOpAttrReplaced;
            tevP->tenantP        = tenantP;
            tevP->entityId       = entityId;
            tevP->entityType     = etype;
            tevP->attrName       = attrIri;
            tevP->modifiedAtNs   = swRest.requestStartTime;
            tevP->entitySnapshot = merged;
            troeDeferAttrEvent(tevP);
          }
        }
      }
    }
  }

  int errorsCount = 0;
  for (KjNode* p = errorsArrayP->value.firstChildP; p != NULL; p = p->next) errorsCount++;

  if (!anySucceeded && errorsCount == 0)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "entity '%s' not found", entityId);
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

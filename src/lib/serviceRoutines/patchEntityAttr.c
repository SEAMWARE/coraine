//
// FILE            patchEntityAttr.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// PATCH /ngsi-ld/v1/entities/{entityId}/attrs/{attrId}
//
// § 5.6.4 Partial Attribute Update. The request body is an Attribute
// Fragment (not an Entity Fragment) — only the sub-fields named by the
// fragment change on the target attribute. Sub-attributes may be deleted
// via the "urn:ngsi-ld:null" marker, but the whole attribute itself
// cannot (§ 5.6.4.1).
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp, strlen, memcpy
#include <stdlib.h>                                  // free
#include <stdio.h>                                   // snprintf

#include "swRest/SwRestState.h"                      // swRest
#include "swRest/SwRestVerb.h"                       // SwVerbPatch

#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjChildAdd, kjChildRemove
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjRender.h"                          // kjFastRender
#include "kjson/kjRenderSize.h"                      // kjFastRenderSize

#include "swJsonld/swldInit.h"                       // SWLD_CORE_CONTEXT_URL
#include "swJsonld/swldExpand.h"                     // swldExpand

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdOp.h"                           // LdOpMergeEntity
#include "swNgsild/ldCheckEntity.h"                  // ldCheckEntity
#include "swNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "swNgsild/ldEntityMerge.h"                  // LdMergeReport
#include "swNgsild/LdVocab.h"                        // LD_VOCAB_SCOPE
#include "swNgsild/LdSubCache.h"                     // LdSubCache
#include "swNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityUpdate
#include "swNgsild/ldNotifyDefer.h"                  // ldNotifyDefer

#include "troe/TroeDriver.h"                         // TroeEvent
#include "troe/troeFromMerge.h"                      // troeDeferAttrEventsFromMerge

#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode, LdRegInfo
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "swNgsild/ldDistOp.h"                       // ldDistOp*

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/patchEntityAttr.h"         // Own interface



// -----------------------------------------------------------------------------
//
// attrUrl - <endpoint>/ngsi-ld/v1/entities/<entityId>/attrs/<attrWild>
//
static char* attrUrl(const char* endpoint, const char* entityId, const char* attrWild)
{
  const char* p1 = "/ngsi-ld/v1/entities/";
  const char* p2 = "/attrs/";

  int  lenE = strlen(endpoint);
  int  lenP1 = strlen(p1);
  int  lenId = strlen(entityId);
  int  lenP2 = strlen(p2);
  int  lenA  = strlen(attrWild);

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



// -----------------------------------------------------------------------------
//
// renderBodyWithContext -
//
static char* renderBodyWithContext(KjNode* bodyP)
{
  // Strip body @context: forward goes out as application/json + Link.
  KjNode* atCtx = kjLookup(bodyP, "@context");
  if (atCtx != NULL)
    kjChildRemove(bodyP, atCtx);

  int   bufSize = kjFastRenderSize(bodyP) + 1;
  char* buf     = (char*) kaAlloc(&swRest.kalloc, bufSize);
  kjFastRender(bodyP, buf);
  return buf;
}



// -----------------------------------------------------------------------------
//
// patchEntityAttr -
//
bool patchEntityAttr(void)
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

  SwldContext* ctxP    = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();
  const char*  attrIri = swldExpand(ctxP, attrWild, &swRest.kalloc, NULL, NULL);
  if (attrIri == NULL) attrIri = attrWild;

  //
  // § 5.6.4.4: "If the target Attribute is scope, then an error of type
  // BadRequestData shall be raised."
  //
  if (strcmp(attrWild, "scope") == 0 || strcmp(attrIri, LD_VOCAB_SCOPE) == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "scope cannot be modified via Partial Attribute Update");
    return true;
  }

  //
  // @context (if in body, as for application/ld+json) was used by the
  // parseHook to expand the tree; remove before re-using the body as an
  // attribute fragment (it is not an attribute sub-field).
  //
  KjNode* ctxNodeP = kjLookup(bodyP, "@context");
  if (ctxNodeP != NULL) kjChildRemove(bodyP, ctxNodeP);

  //
  // Wrap the attribute fragment into a fake entity fragment so we can
  // reuse the entity-level merge path.
  //
  KjNode* entityFrag = kjObject(swRest.kjsonP, NULL);
  KjNode* attrCopy   = bodyP;
  attrCopy->name     = (char*) attrIri;
  kjChildAdd(entityFrag, attrCopy);

  //
  // Validate as a Merge Entity fragment (permits partial, allows null-markers).
  //
  if (ldCheckEntity(entityFrag, LdOpMergeEntity, NULL, &swRest.kalloc) == false)
    return true;
  //
  // § 9.3.3 guard — a ?local=true write must not produce local data that an
  // exclusive or redirect registration claims.
  //
  if (swNgsild.local == true && ((Tenant*) swNgsild.tenantP)->regCacheP != NULL)
  {
    const char* cRegId = ldRegCacheLocalWriteConflictTree((LdRegCache*) ((Tenant*) swNgsild.tenantP)->regCacheP,
                                                          entityId, entityFrag, &swRest.kalloc);
    if (cRegId != NULL)
    {
      ldError(409, LD_ERROR_ALREADY_EXISTS, "Conflict",
              "local update overlaps with registration '%s' (§ 9.3.3 — no local data for an exclusive/redirect scope)",
              cRegId);
      return true;
    }
  }


  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  KjNode* errorsArrayP = kjArray(swRest.kjsonP, "errors");
  bool    anySucceeded = false;

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

  bool dispatch = (swNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;

  //
  // DistOps — forward the raw attribute fragment to each matching CSR.
  // Op name: "updateAttrs". exclusive/redirect detach the attribute
  // from local processing (entityFrag -> empty); inclusive keeps it.
  //
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
                                  swRest.serviceP->ldOp, "updateAttrs",
                                  entityId, /*perRi=*/true, entityId, attrIri,
                                  errorsArrayP, &items);

    for (int i = 0; i < n; i++)
    {
      KjNode* fwdBody = kjObject(swRest.kjsonP, NULL);
      for (KjNode* c = attrCopy->value.firstChildP; c != NULL; c = c->next)
        kjChildAdd(fwdBody, c);

      char* bodyStr = renderBodyWithContext(fwdBody);
      items[i].url     = attrUrl(items[i].csr->endpoint, entityId, attrWild);
      items[i].body    = bodyStr;
      items[i].bodyLen = strlen(bodyStr);
    }

    ldDistOpEntriesPerform(items, n, SwVerbPatch, ownAlias);

    for (int i = 0; i < n; i++)
    {
      int sc = items[i].statusCode;
      if (sc >= 200 && sc < 300)
      {
        anySucceeded = true;
        if (detach[items[i].modeIdx]) localApply = false;
      }
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
  // Local apply — db.entityMerge does RFC 7396 (JSON Merge Patch).
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
      KjNode* existingAttr = kjLookup(targetEntity, attrIri);
      if (existingAttr == NULL)
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
        // § 5.6.4.4: "If present in the provided NGSI-LD Entity Fragment,
        // the type of the Attribute has to be the same as the type of
        // the targeted Attribute fragment". Storage form puts the type
        // on each instance object — check the first instance we find.
        KjNode* fragTypeP = kjLookup(bodyP, "type");
        if (fragTypeP != NULL && fragTypeP->type == KjString && existingAttr->type == KjObject)
        {
          KjNode* anyInstP = existingAttr->value.firstChildP;
          KjNode* existingTypeP = (anyInstP != NULL) ? kjLookup(anyInstP, "type") : NULL;
          if (existingTypeP != NULL && existingTypeP->type == KjString &&
              strcmp(fragTypeP->value.s, existingTypeP->value.s) != 0)
          {
            ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
                    "attribute type mismatch: cannot change '%s' from '%s' to '%s'",
                    attrWild, existingTypeP->value.s, fragTypeP->value.s);
            return true;
          }
        }

        // § 5.6.4: the target instance must already exist. Default
        // instance (no datasetId in the fragment) requires "@none" in
        // storage; a fragment with datasetId X requires X as a key.
        // Returning 204 in either no-match case would silently create
        // a default — that's a merge of a non-existent attribute and
        // is reported as 404 by 012_03_02 / 012_03_03.
        if (existingAttr->type == KjObject)
        {
          KjNode* dsP = kjLookup(bodyP, "datasetId");
          const char* key = (dsP != NULL && dsP->type == KjString)
                              ? dsP->value.s : "@none";
          if (kjLookup(existingAttr, key) == NULL && !anySucceeded)
          {
            if (dsP != NULL && dsP->type == KjString)
              ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                      "no instance of attribute '%s' with datasetId '%s' on entity '%s'",
                      attrWild, dsP->value.s, entityId);
            else
              ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                      "no default instance of attribute '%s' on entity '%s'",
                      attrWild, entityId);
            return true;
          }
        }

        ldApiEntityToDbModel(entityFrag, &swRest.kalloc);

        LdMergeReport report = { NULL };
        int r = db.entityMerge(tenantP, entityId, entityFrag,
                                swRest.requestStartTime, &report);

        if (r != DB_OK && r != DB_NOT_FOUND)
        {
          ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
                  "database error updating attribute '%s' on entity '%s'",
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

          // TRoE: defer one attr event per top-level attr in the merge report.
          if (merged == NULL)
            db.entityRetrieve(tenantP, entityId, &merged);
          {
            const char* etype = NULL;
            if (merged != NULL)
            {
              KjNode* tn = kjLookup(merged, "type");
              if (tn != NULL && tn->type == KjString) etype = tn->value.s;
            }
            troeDeferAttrEventsFromMerge(tenantP, entityId, etype, merged, &report,
                                         swRest.requestStartTime);
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

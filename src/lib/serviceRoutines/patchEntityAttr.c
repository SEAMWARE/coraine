//
// FILE            patchEntityAttr.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
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

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/CorRestVerb.h"                       // CorVerbPatch

#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjChildAdd, kjChildRemove
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjClone.h"                           // kjClone
#include "kjson/kjRender.h"                          // kjFastRender
#include "kjson/kjRenderSize.h"                      // kjFastRenderSize

#include "corJsonld/corLdInit.h"                       // CORLD_CORE_CONTEXT_URL
#include "corJsonld/corLdExpand.h"                     // corLdExpand
#include "corJsonld/corLdCompactTree.h"                // corLdCompactTreeWith
#include "corNgsild/ldQRender.h"                      // ldCompactOrEncode

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdOp.h"                           // LdOpMergeEntity
#include "corNgsild/ldCheckEntity.h"                  // ldCheckEntity
#include "corNgsild/ldCheckAttribute.h"               // ldCheckAttribute
#include "corNgsild/ldAttrTypeDetect.h"               // ldAttrTypeDetect
#include "corNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "corNgsild/ldEntityMerge.h"                  // LdMergeReport
#include "corNgsild/LdVocab.h"                        // LD_VOCAB_SCOPE
#include "corNgsild/LdSubCache.h"                     // LdSubCache
#include "corNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityUpdate
#include "corNgsild/ldNotifyDefer.h"                  // ldNotifyDefer

#include "troe/TroeDriver.h"                         // TroeEvent
#include "troe/troeFromMerge.h"                      // troeDeferAttrEventsFromMerge

#include "corNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode, LdRegInfo
#include "corNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "corNgsild/ldDistOp.h"                       // ldDistOp*

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

  char* buf = (char*) kaAlloc(&corRest.kalloc, lenE + lenP1 + lenId + lenP2 + lenA + 1);
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
  char* buf     = (char*) kaAlloc(&corRest.kalloc, bufSize);
  kjFastRender(bodyP, buf);
  return buf;
}



// -----------------------------------------------------------------------------
//
// patchEntityAttr -
//
bool patchEntityAttr(void)
{
  const char* entityId = corRest.in.wildcard[0];
  const char* attrWild = corRest.in.wildcard[1];
  KjNode*     bodyP    = corRest.in.requestTree;

  if (bodyP->type != KjObject)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Not a JSON Object",
            "attribute fragment must be a JSON object");
    return true;
  }

  CorLdContext* ctxP    = (corNgsild.contextP != NULL) ? corNgsild.contextP : corLdCoreContext();
  const char*  attrIri = corLdExpand(ctxP, attrWild, &corRest.kalloc, NULL, NULL);
  if (attrIri == NULL) attrIri = attrWild;

  //
  // § 5.6.4.4: "If the target Attribute is scope, then an error of type
  // BadRequestData shall be raised."
  //
  if (strcmp(attrWild, "scope") == 0 || strcmp(attrIri, LD_VOCAB_SCOPE) == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Immutable Field",
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
  // Wrapped-fragment form — {"<attrName>": {...}}: the ETSI suite (and
  // forwards built from such requests) sends the name-keyed form rather
  // than the bare Attribute Fragment of § 5.3. Accept it: unwrap for
  // local processing. Forwards mirror the incoming shape (fwdSrcP).
  //
  KjNode* fwdSrcP = bodyP;
  KjNode* soleP   = bodyP->value.firstChildP;
  if (soleP != NULL && soleP->next == NULL && soleP->type == KjObject &&
      soleP->name != NULL && strcmp(soleP->name, attrIri) == 0)
    bodyP = soleP;

  //
  // Wrap the attribute fragment into a fake entity fragment so we can
  // reuse the entity-level merge path.
  //
  KjNode* entityFrag = kjObject(corRest.kjsonP, NULL);
  KjNode* attrCopy   = bodyP;
  attrCopy->name     = (char*) attrIri;
  kjChildAdd(entityFrag, attrCopy);

  //
  // Validate as a Merge Entity fragment (permits partial, allows null-markers).
  //
  if (ldCheckEntity(entityFrag, LdOpMergeEntity, NULL, &corRest.kalloc) == false)
    return true;
  //
  // § 9.3.3 guard — a ?local=true write must not produce local data that an
  // exclusive or redirect registration claims.
  //
  if (corNgsild.local == true && ((Tenant*) corNgsild.tenantP)->regCacheP != NULL)
  {
    const char* cRegId = ldRegCacheLocalWriteConflictTree((LdRegCache*) ((Tenant*) corNgsild.tenantP)->regCacheP,
                                                          entityId, entityFrag, &corRest.kalloc);
    if (cRegId != NULL)
    {
      ldError(409, LD_ERROR_ALREADY_EXISTS, "Conflict",
              "local update overlaps with registration '%s' (§ 9.3.3 — no local data for an exclusive/redirect scope)",
              cRegId);
      return true;
    }
  }


  Tenant* tenantP = (Tenant*) corNgsild.tenantP;

  KjNode* errorsArrayP = kjArray(corRest.kjsonP, "errors");
  bool    anySucceeded = false;

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);

  bool dispatch = (corNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  // Loops handled in the dispatch block (builder marks, ldDistOpLoopReap emits 508).

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
                                  corRest.serviceP->ldOp, "updateAttrs",
                                  entityId, /*perRi=*/true, entityId, attrIri,
                                  errorsArrayP, &items);

    for (int i = 0; i < n; i++)
    {
      // Clone per item — compaction renames nodes in place, and CSRs may
      // compact with different contexts (csi.jsonldContext). Clone from
      // fwdSrcP so the forward mirrors the incoming shape (wrapped or
      // bare) and the local-apply tree stays pristine.
      KjNode*      fwdBody = kjClone(corRest.kjsonP, fwdSrcP);
      CorLdContext* fwdCtx  = ldDistOpForwardContext(items[i].csr);

      corLdCompactTreeWith(fwdBody, fwdCtx);

      char* bodyStr = renderBodyWithContext(fwdBody);

      // The {attrId} path component is an alias too — emit the short the
      // receiver's @context (the one this forward carries) understands;
      // %-encode the IRI when it has no short form there.
      const char* fwdAttr = ldCompactOrEncode(attrIri, fwdCtx, &corRest.kalloc, false);
      items[i].url     = attrUrl(items[i].csr->endpoint, entityId, fwdAttr);
      items[i].body    = bodyStr;
      items[i].bodyLen = strlen(bodyStr);
    }

    n = ldDistOpLoopReap(items, n);

    ldDistOpEntriesPerform(items, n, CorVerbPatch, ownAlias);

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

    ldRegCacheMatchRelease(exclV,  exclN);
    ldRegCacheMatchRelease(redirV, redirN);
    ldRegCacheMatchRelease(inclV,  inclN);
  }

  //
  // Local apply — Partial Attribute Update: broker-side ldEntityFragmentApply
  // (replace/append, not RFC 7396 deep merge) + db.entityChangesApply to persist.
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
            ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Attribute Type Change",
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

        // The attribute already exists, so its type is fixed by the stored
        // entity (a Partial Attribute Update must not change it, § 5.6.4.4). A
        // bare value-only fragment carries no type, so the earlier ldCheckEntity
        // validated it as a plain Property and skipped the type-specific value
        // check (e.g. ldCheckGeo for a GeoProperty). Resolve the type from the
        // DB and validate the value against it here — so a malformed geometry is
        // a 400 in the broker, before it ever reaches the storage layer.
        {
          // Only when the fragment actually carries a value (ldAttrTypeDetect
          // != None) — a sub-attribute-only update (e.g. just observedAt) has
          // no value to validate and must not be forced through the type's
          // value checks.
          KjNode* dbInstP = (existingAttr->type == KjObject) ? existingAttr->value.firstChildP : NULL;
          KjNode* dbTypeP = (dbInstP != NULL) ? kjLookup(dbInstP, "type") : NULL;
          if (dbTypeP != NULL && dbTypeP->type == KjString &&
              kjLookup(bodyP, "type") == NULL && ldAttrTypeDetect(bodyP) != LdAttrNone)
          {
            kjChildAdd(bodyP, kjString(corRest.kjsonP, "type", dbTypeP->value.s));
            if (ldCheckAttribute(bodyP, LdOpMergeEntity, LdAttrNone, &corRest.kalloc) == false)
              return true;
          }
        }

        ldApiEntityToDbModel(entityFrag, &corRest.kalloc, 0);

        //
        // Partial Attribute Update (§ 10.2.5) — replace/append semantics, the
        // value is replaced wholesale, not deep-merged. Apply the fragment to
        // the already-retrieved targetEntity here in the broker, then persist
        // the resulting change report via the driver.
        //
        LdMergeReport report = { NULL };
        if (ldEntityFragmentApply(targetEntity, entityFrag, &report,
                                  corRest.requestStartTime, corRest.kjsonP) == false)
          return true;  // ldError already set

        int car = db.entityChangesApply(tenantP, entityId, targetEntity, &report);
        if (car == DB_GEO_TYPE_CONFLICT)
        {
          ldGeoTypeConflict();
          return true;
        }

        if (car == DB_INVALID_GEOMETRY)
        {
          ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid GeoProperty",
                  "the updated attribute '%s' carries an invalid GeoProperty geometry", attrWild);
          return true;
        }
        if (car != DB_OK)
        {
          ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
                  "database error updating attribute '%s' on entity '%s'",
                  attrWild, entityId);
          return true;
        }

        anySucceeded = true;

        // targetEntity is the post-merge tree — feed notifications + TRoE directly.
        if (tenantP->subCacheP != NULL)
          ldNotifyDefer((LdSubCache*) tenantP->subCacheP, targetEntity,
                        LdNotifyEntityUpdate, &report);

        // TRoE: defer one attr event per top-level attr in the merge report.
        {
          const char* etype = NULL;
          KjNode* tn = kjLookup(targetEntity, "type");
          if (tn != NULL && tn->type == KjString) etype = tn->value.s;
          troeDeferAttrEventsFromMerge(tenantP, entityId, etype, targetEntity, &report,
                                       corRest.requestStartTime);
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
    corRest.out.httpStatusCode = 204;
    return true;
  }

  KjNode* successArrayP = kjArray(corRest.kjsonP, "success");
  if (anySucceeded)
    kjChildAdd(successArrayP, kjString(corRest.kjsonP, NULL, entityId));

  KjNode* respBodyP = kjObject(corRest.kjsonP, NULL);
  kjChildAdd(respBodyP, successArrayP);
  kjChildAdd(respBodyP, errorsArrayP);

  corRest.out.responseTree   = respBodyP;
  corRest.out.httpStatusCode = anySucceeded ? 207 : 409;
  return true;
}

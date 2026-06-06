//
// FILE            deleteEntityAttr.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// DELETE /ngsi-ld/v1/entities/{entityId}/attrs/{attrId}
//
// § 5.6.5 Delete Attribute. Removes an Attribute (or a specific instance)
// from an Entity.
//   - ?deleteAll=true → remove the Attribute and all its instances
//   - ?datasetId=X    → remove only that instance
//   - neither         → remove the default instance ("@none")
//
// Forwarding propagates the same URL (incl. query string) to matching CSRs
// with op "deleteAttrs".
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp, strlen, memcpy, strchr
#include <stdlib.h>                                  // free
#include <stdio.h>                                   // snprintf

#include "swRest/SwRestState.h"                      // swRest
#include "swRest/SwRestVerb.h"                       // SwVerbDelete

#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjString, kjChildAdd, kjArray
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjClone.h"                           // kjClone

#include "swJsonld/swldExpand.h"                     // swldExpand
#include "swJsonld/swldInit.h"                       // swldCoreContext

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdVocab.h"                        // LD_VOCAB_SCOPE, LD_VOCAB_NGSILD_NULL
#include "swNgsild/LdSubCache.h"                     // LdSubCache
#include "swNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityUpdate
#include "swNgsild/ldNotifyDefer.h"                  // ldNotifyDefer
#include "swNgsild/ldEntityMerge.h"                  // LdMergeReport

#include "troe/TroeDriver.h"                         // TroeEvent, TroeOpAttrDeleted
#include "troe/troeDispatch.h"                       // troeDeferAttrEvent

#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode, LdRegInfo
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "swNgsild/ldDistOp.h"                       // ldDistOp*
#include "swNgsild/ldQRender.h"                      // ldCompactOrEncode

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/deleteEntityAttr.h"        // Own interface



// -----------------------------------------------------------------------------
//
// attrUrl - build the forward URL including any ?datasetId / ?deleteAll.
//
static char* attrUrl(const char* endpoint, const char* entityId, const char* attrWild)
{
  const char* p1 = "/ngsi-ld/v1/entities/";
  const char* p2 = "/attrs/";
  const char* dsKey   = swNgsild.datasetId;   // may be NULL
  bool        delAll  = swNgsild.deleteAll;

  int lenE  = strlen(endpoint);
  int lenP1 = strlen(p1);
  int lenId = strlen(entityId);
  int lenP2 = strlen(p2);
  int lenA  = strlen(attrWild);
  int lenDs = (dsKey != NULL) ? strlen(dsKey) : 0;
  int extra = (dsKey != NULL ? 1 + 10 + lenDs : 0) + (delAll ? 1 + 15 : 0);
  //   "?datasetId=" = 11; "?deleteAll=true" = 15; "&..." sep = 1 each.

  char* buf = (char*) kaAlloc(&swRest.kalloc, lenE + lenP1 + lenId + lenP2 + lenA + extra + 1);
  char* p = buf;
  memcpy(p, endpoint, lenE); p += lenE;
  memcpy(p, p1, lenP1);      p += lenP1;
  memcpy(p, entityId, lenId); p += lenId;
  memcpy(p, p2, lenP2);      p += lenP2;
  memcpy(p, attrWild, lenA); p += lenA;

  bool first = true;
  if (dsKey != NULL)
  {
    *p++ = first ? '?' : '&'; first = false;
    memcpy(p, "datasetId=", 10); p += 10;
    memcpy(p, dsKey, lenDs);     p += lenDs;
  }
  if (delAll)
  {
    *p++ = first ? '?' : '&'; first = false;
    memcpy(p, "deleteAll=true", 14); p += 14;
  }
  *p = 0;
  return buf;
}



// -----------------------------------------------------------------------------
//
// applyLocalDelete - remove attr / instance(s) from target, respecting params.
//
// Returns true if any change was made. Populates a report entry the caller
// may use to drive subscription notifications.
//
static bool applyLocalDelete(KjNode* entityP, const char* attrIri)
{
  KjNode* attrP = kjLookup(entityP, attrIri);
  if (attrP == NULL)
    return false;

  if (swNgsild.deleteAll || strcmp(attrIri, LD_VOCAB_SCOPE) == 0)
  {
    kjChildRemove(entityP, attrP);
    return true;
  }

  const char* dsKey = swNgsild.datasetId;
  if (dsKey == NULL) dsKey = "@none";

  KjNode* instP = kjLookup(attrP, dsKey);
  if (instP == NULL)
    return false;

  kjChildRemove(attrP, instP);

  // If the wrapper became empty after removing the only instance, drop it.
  if (attrP->value.firstChildP == NULL)
    kjChildRemove(entityP, attrP);

  return true;
}



// -----------------------------------------------------------------------------
//
// deleteEntityAttr -
//
bool deleteEntityAttr(void)
{
  const char* entityId = swRest.in.wildcard[0];
  const char* attrWild = swRest.in.wildcard[1];

  ldContextResolve();

  SwldContext* ctxP    = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();
  const char*  attrIri = swldExpand(ctxP, attrWild, &swRest.kalloc, NULL, NULL);
  if (attrIri == NULL) attrIri = attrWild;

  //
  // § 5.6.5.4: "If the target Attribute is scope, remove the scope Attribute
  // from the target Entity." No error — "scope" is a legitimate delete target.
  //
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
                                  swRest.serviceP->ldOp, "deleteAttrs",
                                  entityId, /*perRi=*/true, entityId, attrIri,
                                  errorsArrayP, &items);

    // The {attrId} path component is an alias — emit the short the
    // receiver's @context (the one this forward carries) understands;
    // %-encode the IRI when it has no short form there.
    for (int i = 0; i < n; i++)
    {
      const char* fwdAttr = ldCompactOrEncode(attrIri, ldDistOpForwardContext(items[i].csr), &swRest.kalloc);
      items[i].url = attrUrl(items[i].csr->endpoint, entityId, fwdAttr);
    }

    ldDistOpEntriesPerform(items, n, SwVerbDelete, ownAlias);

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
  // Local apply — retrieve entity, mutate in memory, write back via
  // entityReplace. We don't have a DB op dedicated to attribute removal,
  // so a replace with the modified document is the simplest portable way.
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
      if (kjLookup(targetEntity, attrIri) == NULL)
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
        // Snapshot the attribute wrapper BEFORE applyLocalDelete so the
        // showChanges renderer can emit previousValue/Object/Vocab/Json.
        // kjClone runs on the request-scoped kjson; preSnapshot stays valid
        // through db.entityReplace (the kalloc behind both is the same
        // request arena).
        KjNode* preSrc      = kjLookup(targetEntity, attrIri);
        KjNode* preSnapshot = (preSrc != NULL) ? kjClone(swRest.kjsonP, preSrc) : NULL;

        bool changed = applyLocalDelete(targetEntity, attrIri);

        if (changed)
        {
          if (db.entityReplace == NULL)
          {
            ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
                    "Delete Attribute not supported by this DB plugin");
            return true;
          }

          KjNode* oldEntity = NULL;
          int r = db.entityReplace(tenantP, entityId, targetEntity, &oldEntity);

          if (r != DB_OK && r != DB_NOT_FOUND)
          {
            ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
                    "database error deleting attribute '%s' on entity '%s'",
                    attrWild, entityId);
            return true;
          }

          if (r == DB_OK)
          {
            anySucceeded = true;

            if (tenantP->subCacheP != NULL)
            {
              // Build a minimal merge report so the subscription matcher
              // can recognise this as an attributeDeleted change (otherwise
              // a sub with notificationTrigger=["attributeDeleted"] would
              // never fire on attr-delete).
              //
              // Per § 5.8.6: when only one dataset instance was removed
              // (datasetId variant or @none default with deleteAll=false),
              // record the deleted dsKey on the report entry so the
              // notification renderer can inject a per-instance null marker
              // alongside the surviving instances.
              const char* deletedDsKey = NULL;
              if (!swNgsild.deleteAll)
                deletedDsKey = (swNgsild.datasetId != NULL) ? swNgsild.datasetId : "@none";

              LdMergeReport report;
              report.changes = kjArray(swRest.kjsonP, "changes");
              KjNode* entry = kjObject(swRest.kjsonP, NULL);
              kjChildAdd(entry, kjString(swRest.kjsonP, "attr",   attrIri));
              kjChildAdd(entry, kjString(swRest.kjsonP, "reason", "attributeDeleted"));
              if (deletedDsKey != NULL)
                kjChildAdd(entry, kjString(swRest.kjsonP, "datasetId", deletedDsKey));
              if (preSnapshot != NULL)
              {
                preSnapshot->name = (char*) "preValue";
                kjChildAdd(entry, preSnapshot);
              }
              kjChildAdd(report.changes, entry);

              ldNotifyDefer((LdSubCache*) tenantP->subCacheP, targetEntity,
                            LdNotifyEntityUpdate, &report);
            }

            // TRoE: defer one attrDeleted event.
            {
              const char* etype = NULL;
              if (targetEntity != NULL)
              {
                KjNode* tn = kjLookup(targetEntity, "type");
                if (tn != NULL && tn->type == KjString) etype = tn->value.s;
              }
              TroeEvent* tevP = (TroeEvent*) kaAlloc(&swRest.kalloc, sizeof(TroeEvent));
              memset(tevP, 0, sizeof(*tevP));
              tevP->op             = TroeOpAttrDeleted;
              tevP->tenantP        = tenantP;
              tevP->entityId       = entityId;
              tevP->entityType     = etype;
              tevP->attrName       = attrIri;
              tevP->modifiedAtNs   = swRest.requestStartTime;
              tevP->entitySnapshot = targetEntity;
              tevP->attrSnapshot   = preSnapshot;  // pre-delete wrapper — carries the attr kind for the tombstone row
              troeDeferAttrEvent(tevP);
            }
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

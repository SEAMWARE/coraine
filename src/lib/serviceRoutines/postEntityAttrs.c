//
// FILE            postEntityAttrs.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/entities/{entityId}/attrs — Append Attributes (§ 5.6.3).
//

#include <stddef.h>                                   // NULL
#include <string.h>                                   // strcmp, strlen, strcpy, memcpy
#include <stdlib.h>                                   // free
#include <stdio.h>                                    // snprintf
#include <regex.h>                                    // regexec

#include "swRest/SwRestState.h"                       // swRest
#include "swRest/SwRestVerb.h"                        // SwVerbPost
#include "kalloc/kaAlloc.h"                           // kaAlloc
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjObject, kjArray, kjString, kjChildAdd, kjChildRemove
#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjRender.h"                           // kjFastRender
#include "kjson/kjRenderSize.h"                       // kjFastRenderSize

#include "swJsonld/swldCompact.h"                     // swldCompact
#include "swJsonld/swldInit.h"                        // swldCoreContext, SWLD_CORE_CONTEXT_URL

#include "swNgsild/swNgsild.h"                        // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldCheckEntity.h"                   // ldCheckEntity
#include "swNgsild/LdOp.h"                            // LdOpAppendAttrs
#include "swNgsild/ldApiEntityToDbModel.h"            // ldApiEntityToDbModel
#include "swNgsild/ldEntityMerge.h"                   // LdMergeReport
#include "swNgsild/LdVocab.h"                         // LD_VOCAB_*
#include "swNgsild/LdSubCache.h"                      // LdSubCache
#include "swNgsild/ldSubscriptionNotify.h"            // LdNotifyEntityUpdate
#include "swNgsild/ldNotifyDefer.h"                   // ldNotifyDefer

#include "troe/troeFromMerge.h"                       // troeDeferAttrEventsFromMerge

#include "swNgsild/LdRegCache.h"                      // LdRegCache, LdRegCacheItem, LdRegMode, LdRegInfo
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "swNgsild/ldDistOp.h"                        // ldDistOpLoopDetected, ldDistOpSend, ldDistOpCsrWouldLoop
#include "swNgsild/ldEntityFragment.h"                // ldEntityFragmentForInfo
#include "swNgsild/ldWriteResult.h"                   // LdWriteResult, ldWriteResultMerge, ldWriteResult*Add

#include "db/DbDriver.h"                              // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                                // Tenant

#include "ktrace/kTrace.h"                            // KT_T
#include "swBrokerTraceLevels.h"                      // KtDistOpRequest

#include "serviceRoutines/postEntityAttrs.h"          // Own interface



// -----------------------------------------------------------------------------
//
// isEntityKeyword - top-level node names that are not attributes
//
static bool isEntityKeyword(const char* name)
{
  if (name == NULL)              return true;
  if (name[0] == '@')            return true;
  if (strcmp(name, "id")   == 0) return true;
  if (strcmp(name, "type") == 0) return true;
  if (strcmp(name, LD_VOCAB_SCOPE) == 0) return true;
  return false;
}



// -----------------------------------------------------------------------------
//
// shortNameOf - compact a (possibly expanded) attribute name for the response
//
static const char* shortNameOf(const char* attrIri)
{
  const char* compact = swldCompact(swldCoreContext(), attrIri);
  return (compact != NULL) ? compact : attrIri;
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
// attrsUrl - compose the POST forward URL for one CSR
//
//   <endpoint>/ngsi-ld/v1/entities/<entityId>/attrs[?options=noOverwrite]
//
static char* attrsUrl(const char* endpoint, const char* entityId, bool noOverwrite)
{
  const char* path    = "/ngsi-ld/v1/entities/";
  const char* suffix  = "/attrs/";   // trailing slash matches the § 6.6.3 spec
                                     // URI template — and the ETSI HttpCtrl
                                     // stub matcher is strict on URL, so a
                                     // stubbed `…/attrs/` won't match `…/attrs`.
  const char* qs      = noOverwrite ? "?options=noOverwrite" : "";
  int         baseLen = strlen(endpoint);
  int         pathLen = strlen(path);
  int         idLen   = strlen(entityId);
  int         sufLen  = strlen(suffix);
  int         qsLen   = strlen(qs);
  char*       url     = (char*) kaAlloc(&swRest.kalloc,
                                         baseLen + pathLen + idLen + sufLen + qsLen + 1);
  char*       p       = url;
  memcpy(p, endpoint, baseLen); p += baseLen;
  memcpy(p, path,     pathLen); p += pathLen;
  memcpy(p, entityId, idLen);   p += idLen;
  memcpy(p, suffix,   sufLen);  p += sufLen;
  memcpy(p, qs,       qsLen);   p += qsLen;
  *p = 0;
  return url;
}



// -----------------------------------------------------------------------------
//
// renderFragmentWithContext - serialize fragment with @context for remote
//
static char* renderFragmentWithContext(KjNode* fragP)
{
  // Strip body @context: forward goes out as application/json + Link.
  KjNode* atCtx = kjLookup(fragP, "@context");
  if (atCtx != NULL)
    kjChildRemove(fragP, atCtx);

  int   bufSize = kjFastRenderSize(fragP) + 1;
  char* buf     = (char*) kaAlloc(&swRest.kalloc, bufSize);
  kjFastRender(fragP, buf);
  return buf;
}



// -----------------------------------------------------------------------------
//
// classifyAndChopLocal - noOverwrite filter against local existing entity
//
// For the (possibly chopped) fragment remaining after distops, apply
// noOverwrite semantics vs the local entity: strip conflicting
// instances, record them in notUpdatedP, and emit updated[] for what
// survives.
//
static void classifyAndChopLocal(KjNode* fragment, KjNode* existing, bool noOverwrite,
                                 KjNode* updatedP, KjNode* notUpdatedP)
{
  if (fragment == NULL || existing == NULL) return;

  KjNode* fAttrP = fragment->value.firstChildP;
  while (fAttrP != NULL)
  {
    KjNode* nextAttr = fAttrP->next;
    if (isEntityKeyword(fAttrP->name) || fAttrP->type != KjObject)
    {
      fAttrP = nextAttr;
      continue;
    }

    KjNode*     tAttrP    = kjLookup(existing, fAttrP->name);
    const char* shortName = shortNameOf(fAttrP->name);
    bool        anyConflict = false;
    bool        anyKept     = false;

    if (tAttrP != NULL)
    {
      KjNode* fInstP = fAttrP->value.firstChildP;
      while (fInstP != NULL)
      {
        KjNode* nextInst = fInstP->next;
        if (fInstP->type == KjObject)
        {
          KjNode* tInstP = kjLookup(tAttrP, fInstP->name);
          if (tInstP != NULL)
          {
            anyConflict = true;
            if (noOverwrite)
            {
              kjChildRemove(fAttrP, fInstP);
              fInstP = nextInst;
              continue;
            }
          }
        }
        anyKept = true;
        fInstP = nextInst;
      }
    }
    else
    {
      anyKept = true;
    }

    if (noOverwrite && anyConflict)
      ldWriteResultNotUpdatedAdd(notUpdatedP, shortName, "Attribute already exists", NULL, 0);

    if (!anyKept)
      kjChildRemove(fragment, fAttrP);
    else
      ldWriteResultUpdatedAdd(updatedP, shortName);

    fAttrP = nextAttr;
  }
}



// -----------------------------------------------------------------------------
//
// postEntityAttrs -
//
bool postEntityAttrs(void)
{
  const char* entityId = swRest.in.wildcard[0];
  KjNode*     fragment = swRest.in.requestTree;

  if (ldCheckEntity(fragment, LdOpAppendAttrs, NULL, &swRest.kalloc) == false)
    return true;

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;
  bool    noOverwrite = swNgsild.noOverwrite;
  //
  // § 9.3.3 guard — a ?local=true write must not produce local data that an
  // exclusive or redirect registration claims.
  //
  if (swNgsild.local == true && tenantP->regCacheP != NULL)
  {
    const char* cRegId = ldRegCacheLocalWriteConflictTree((LdRegCache*) tenantP->regCacheP,
                                                          entityId, fragment, &swRest.kalloc);
    if (cRegId != NULL)
    {
      ldError(409, LD_ERROR_ALREADY_EXISTS, "Conflict",
              "local update overlaps with registration '%s' (§ 9.3.3 — no local data for an exclusive/redirect scope)",
              cRegId);
      return true;
    }
  }


  //
  // DistOps dispatch (§ 5.6.3.4). Skipped on ?local=true, no regCache,
  // or Via loop. exclusive/redirect chop, inclusive clones.
  //
  // Fragment is in API form. ldEntityFragmentForInfo extracts per-CSR
  // slices (whose attrs match the CSR's attributeNames)
  // and, when detach=true, removes them from the original fragment so
  // exclusive-claimed attrs don't leak to the local path. Forward body
  // is the slice rendered with @context; CSR endpoint gets a POST to
  // <endpoint>/ngsi-ld/v1/entities/<id>/attrs (?options=noOverwrite
  // preserved).
  //
  KjNode* updatedP    = kjArray(swRest.kjsonP, "updated");
  KjNode* notUpdatedP = kjArray(swRest.kjsonP, "notUpdated");

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

  bool dispatch = (swNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  // A loop (§ 9.7) no longer skips dispatch: exclusive/redirect attrs are
  // chopped and recorded as loop-blocked rather than forwarded (§ 6.3.18); an
  // all-external loop flattens the terminal 404 below to 508.
  bool loopSeen    = (dispatch && ldDistOpLoopDetected(ownAlias));
  bool loopBlocked = false;

  bool anyCsrSucceeded = false;

  if (dispatch)
  {
    KjNode* typeP = kjLookup(fragment, "type");
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

    KT_T(KtDistOpRequest, "postEntityAttrs dispatch: entityId=%s type=%s excl=%d redir=%d incl=%d",
         entityId, typeArgP != NULL ? typeArr[0] : "(none)", exclN, redirN, inclN);

    LdRegCacheItem** groups[]  = { exclV,       redirV,     inclV      };
    int              counts[]  = { exclN,       redirN,     inclN      };
    const char*      modeTag[] = { "exclusive", "redirect", "inclusive" };
    bool             opConf[]  = { true,        true,       false      };

    int total = 0;
    for (int g = 0; g < 3; g++)
      for (int i = 0; i < counts[g]; i++)
        for (LdRegInfo* riP = groups[g][i]->infoV; riP != NULL; riP = riP->next) total++;

    LdDistOpBatchItem*   items   = (LdDistOpBatchItem*)   kaAlloc(&swRest.kalloc, total * sizeof(LdDistOpBatchItem));
    memset(items, 0, total * sizeof(LdDistOpBatchItem));
    LdDistOpBatchResult* results = (LdDistOpBatchResult*) kaAlloc(&swRest.kalloc, total * sizeof(LdDistOpBatchResult));
    KjNode**             itemFrag = (KjNode**) kaAlloc(&swRest.kalloc, total * sizeof(KjNode*));
    int                  itemCount = 0;
    memset(results, 0, total * sizeof(LdDistOpBatchResult));

    //
    // groups[] is iterated exclusive → redirect → inclusive.
    //   * Exclusive (g==0): each CSR owns its claimed attrs uniquely;
    //     detach as we go.
    //   * Redirect (g==1): multiple redirect CSRs covering the same
    //     entity must ALL receive the same fragment; clone here and
    //     do one detach sweep after the loop.
    //   * Inclusive (g==2): clone — local merge keeps them.
    //
    for (int g = 0; g < 3; g++)
    {
      for (int i = 0; i < counts[g]; i++)
      {
        LdRegCacheItem* csr = groups[g][i];
        if (csr->endpoint == NULL)                    continue;
        bool loop = loopSeen || ldDistOpCsrWouldLoop(csr, ownAlias);

        bool opSupported = ldRegOpSupported(csr, swRest.serviceP->ldOp);

        for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
        {
          if (!entityInfoCoversId(riP, entityId)) continue;

          KjNode* fragP = ldEntityFragmentForInfo(fragment, riP, swRest.kjsonP, /*detach=*/(g == 0));
          if (fragP == NULL) continue;

          if (!opSupported)
          {
            if (!opConf[g]) continue;
            char reason[256];
            snprintf(reason, sizeof(reason),
                     "%s registration does not support appendAttrs", modeTag[g]);
            ldWriteResultFragNotUpdated(notUpdatedP, fragP, reason, csr->regId, 0);
            continue;
          }

          // Loop-blocked forward (§ 6.3.18): fragP's attrs are chopped, so for
          // excl/redirect record them as not-updated (loop) and remember it so
          // an all-external loop flattens to 508; inclusive keeps its clone.
          if (loop)
          {
            if (opConf[g])
            {
              loopBlocked = true;
              ldWriteResultFragNotUpdated(notUpdatedP, fragP,
                                          "loop detected: registration resolves back to this broker", csr->regId, 0);
            }
            continue;
          }

          char* body = renderFragmentWithContext(fragP);
          items[itemCount].csr     = csr;
          items[itemCount].url     = attrsUrl(csr->endpoint, entityId, noOverwrite);
          items[itemCount].body    = body;
          items[itemCount].bodyLen = strlen(body);
          KT_T(KtDistOpRequest, "forward: POST %s", items[itemCount].url);
          itemFrag[itemCount]      = fragP;
          itemCount++;
        }
      }
    }

    // Post-loop redirect-detach: strip from the source fragment every
    // attribute the redirect group cloned, so the local store below
    // only sees what's left.
    for (int i = 0; i < counts[1]; i++)
    {
      LdRegCacheItem* csr = groups[1][i];
      if (csr->endpoint == NULL)                  continue;
      // Detach redirect attrs whether forwarded or loop-blocked — a
      // redirect-owned attr must not be stored locally either way.
      for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
      {
        if (!entityInfoCoversId(riP, entityId)) continue;
        KjNode* drop = ldEntityFragmentForInfo(fragment, riP, swRest.kjsonP, /*detach=*/true);
        (void) drop;
      }
    }

    if (itemCount > 0)
    {
      ldDistOpSendMulti(items, itemCount, SwVerbPost, ownAlias, results);

      // Merge every CSR response into one UpdateResult. A CSR's 207 carries its
      // own partial result and must be folded in (not swallowed as a clean 2xx);
      // a 404 from an inclusive/redirect leg is benign. tolerate404=true keeps the
      // long-standing idempotent reading.
      LdWriteResult wr;
      ldWriteResultInit(&wr, updatedP, notUpdatedP);

      for (int i = 0; i < itemCount; i++)
      {
        KT_T(KtDistOpRequest,
             "forward result %d/%d: url=%s status=%d errorDetail=%s bodyLen=%d",
             i + 1, itemCount, items[i].url, results[i].statusCode,
             (results[i].errorDetail != NULL ? results[i].errorDetail : "(none)"),
             results[i].responseBodyLen);

        ldWriteResultMerge(&wr, items[i].csr->regId,
                           results[i].statusCode, results[i].errorDetail,
                           results[i].responseTree,
                           itemFrag[i], /* tolerate404 = */ true);
      }

      if (wr.anyOk)
        anyCsrSucceeded = true;
    }

    ldRegCacheMatchRelease(exclV,  exclN);
    ldRegCacheMatchRelease(redirV, redirN);
    ldRegCacheMatchRelease(inclV,  inclN);
  }

  //
  // Local path — what's left of the fragment (exclusive/redirect chop
  // removed their claims). If the fragment still has attrs, try local;
  // otherwise skip.
  //
  // localHasAttrs is true when the fragment carries anything the local
  // DB layer needs to persist — attributes OR a scope update. type and
  // id are intentionally NOT counted (POST /attrs doesn't add types,
  // § 5.6.3 §  4.5.6, and id is immutable). `scope` flows through
  // ldEntityAttrsSet::applyScope and respects ?options=noOverwrite.
  bool localHasAttrs = false;
  for (KjNode* c = fragment->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->name == NULL)               continue;
    if (c->name[0] == '@')             continue;
    if (strcmp(c->name, "id")   == 0)  continue;
    if (strcmp(c->name, "type") == 0)  continue;
    localHasAttrs = true;
    break;
  }

  KjNode* existing = NULL;
  int     rr       = DB_NOT_FOUND;
  if (localHasAttrs || !anyCsrSucceeded)
    rr = db.entityRetrieve(tenantP, entityId, &existing);

  if (rr != DB_OK && !anyCsrSucceeded)
  {
    // Entity unknown locally AND no CSR landed. If a loop-blocked exclusive/
    // redirect registration consumed the request, the data is held externally
    // and unreachable via the loop → 508 (§ 6.3.18); else 404 per § 5.6.3.4.
    if (loopBlocked)
      ldError(508, LD_ERROR_LOOP_DETECTED, "Loop Detected",
              "loop detected: entity '%s' is held by an exclusive/redirect registration that resolves back to this broker", entityId);
    else
      ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity '%s' not found", entityId);
    return true;
  }

  if (localHasAttrs && rr == DB_OK && existing != NULL)
  {
    ldApiEntityToDbModel(fragment, &swRest.kalloc, 0);
    classifyAndChopLocal(fragment, existing, noOverwrite, updatedP, notUpdatedP);

    if (db.entityAttrsSet == NULL)
    {
      ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
              "Append Attributes not supported by this DB plugin");
      return true;
    }

    bool overwriteScope = !noOverwrite;
    LdMergeReport report = { NULL };
    int r = db.entityAttrsSet(tenantP, entityId, fragment, overwriteScope,
                               swRest.requestStartTime, &report);

    if (r != DB_OK && r != DB_NOT_FOUND)
    {
      ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
              "database error appending to entity '%s'", entityId);
      return true;
    }

    if (r == DB_OK)
    {
      KjNode* mergedEntity = NULL;
      if (tenantP->subCacheP != NULL)
        db.entityRetrieve(tenantP, entityId, &mergedEntity);

      if (tenantP->subCacheP != NULL && mergedEntity != NULL)
        ldNotifyDefer((LdSubCache*) tenantP->subCacheP, mergedEntity, LdNotifyEntityUpdate, &report);

      // TRoE: defer one attr event per top-level attr in the merge report.
      if (mergedEntity == NULL)
        db.entityRetrieve(tenantP, entityId, &mergedEntity);
      {
        const char* etype = NULL;
        if (mergedEntity != NULL)
        {
          KjNode* tn = kjLookup(mergedEntity, "type");
          if (tn != NULL && tn->type == KjString) etype = tn->value.s;
        }
        troeDeferAttrEventsFromMerge(tenantP, entityId, etype, mergedEntity, &report,
                                     swRest.requestStartTime);
      }
    }
  }

  //
  // Response:
  //   - notUpdated[] empty → 204.
  //   - otherwise → 207 Multi-Status + UpdateResult body (§ 5.2.18).
  //
  int notUpdatedCount = 0;
  for (KjNode* p = notUpdatedP->value.firstChildP; p != NULL; p = p->next) notUpdatedCount++;

  if (notUpdatedCount == 0)
  {
    swRest.out.httpStatusCode = 204;
    return true;
  }

  KjNode* respBodyP = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(respBodyP, updatedP);
  kjChildAdd(respBodyP, notUpdatedP);

  swRest.out.responseTree   = respBodyP;
  swRest.out.httpStatusCode = 207;
  return true;
}

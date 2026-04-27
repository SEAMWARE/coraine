//
// FILE            patchEntityAttrs.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// PATCH /ngsi-ld/v1/entities/{entityId}/attrs — Update Attributes (§ 5.6.2).
//
// Close cousin of postEntityAttrs (Append). Differences:
//   - No ?options=noOverwrite (spec defines this for Append only).
//   - "urn:ngsi-ld:null" as attr value → delete the attr (at top level)
//     or delete the dsKey instance (inside a wrapper).
//   - DistOps op name is "updateAttrs".
//
// Everything else — UpdateResult body shape, 204/207/404 matrix,
// chop-and-forward for exclusive/redirect CSRs, ?local bypass,
// proactive loop-skip — is the same.
//

#include <stddef.h>                                   // NULL
#include <string.h>                                   // strcmp, strlen, memcpy
#include <stdlib.h>                                   // free
#include <stdio.h>                                    // snprintf
#include <regex.h>                                    // regexec

#include "swRest/SwRestState.h"                       // swRest
#include "swRest/SwRestVerb.h"                        // SwVerbPatch
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
#include "swNgsild/LdOp.h"                            // LdOpUpdateEntity
#include "swNgsild/ldApiEntityToDbModel.h"            // ldApiEntityToDbModel
#include "swNgsild/ldEntityMerge.h"                   // LdMergeReport
#include "swNgsild/LdVocab.h"                         // LD_VOCAB_*
#include "swNgsild/LdSubCache.h"                      // LdSubCache
#include "swNgsild/ldSubscriptionNotify.h"            // LdNotifyEntityUpdate
#include "swNgsild/ldNotifyDefer.h"                   // ldNotifyDefer

#include "troe/troeFromMerge.h"                       // troeDeferAttrEventsFromMerge

#include "swNgsild/LdRegCache.h"                      // LdRegCache, LdRegCacheItem, LdRegMode, LdRegInfo
#include "swNgsild/ldRegCache.h"                      // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "swNgsild/ldCsourceAlias.h"                  // ldCsourceAliasForTenant
#include "swNgsild/ldDistOp.h"                        // ldDistOpLoopDetected, ldDistOpSend, ldDistOpCsrWouldLoop
#include "swNgsild/ldEntityFragment.h"                // ldEntityFragmentForInfo

#include "db/DbDriver.h"                              // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                                // Tenant

#include "serviceRoutines/patchEntityAttrs.h"         // Own interface



// -----------------------------------------------------------------------------
//
// isEntityKeyword -
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
// shortNameOf -
//
static const char* shortNameOf(const char* attrIri)
{
  const char* compact = swldCompact(swldCoreContext(), attrIri);
  return (compact != NULL) ? compact : attrIri;
}



static void addNotUpdated(KjNode* arrP, const char* attrName,
                          const char* reason, const char* regId)
{
  KjNode* entry = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(entry, kjString(swRest.kjsonP, "attributeName", attrName));
  kjChildAdd(entry, kjString(swRest.kjsonP, "reason",        reason));
  if (regId != NULL)
    kjChildAdd(entry, kjString(swRest.kjsonP, "registrationId", regId));
  kjChildAdd(arrP, entry);
}

static void addUpdatedUnique(KjNode* arrP, const char* attrName)
{
  for (KjNode* p = arrP->value.firstChildP; p != NULL; p = p->next)
    if (p->type == KjString && strcmp(p->value.s, attrName) == 0)
      return;
  kjChildAdd(arrP, kjString(swRest.kjsonP, NULL, attrName));
}



// -----------------------------------------------------------------------------
//
// entityInfoCoversId -
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
// attrsUrl -
//
static char* attrsUrl(const char* endpoint, const char* entityId)
{
  const char* path    = "/ngsi-ld/v1/entities/";
  const char* suffix  = "/attrs";
  int         baseLen = strlen(endpoint);
  int         pathLen = strlen(path);
  int         idLen   = strlen(entityId);
  int         sufLen  = strlen(suffix);
  char*       url     = (char*) kaAlloc(&swRest.kalloc,
                                         baseLen + pathLen + idLen + sufLen + 1);
  char*       p       = url;
  memcpy(p, endpoint, baseLen); p += baseLen;
  memcpy(p, path,     pathLen); p += pathLen;
  memcpy(p, entityId, idLen);   p += idLen;
  memcpy(p, suffix,   sufLen);  p += sufLen;
  *p = 0;
  return url;
}



// -----------------------------------------------------------------------------
//
// renderFragmentWithContext -
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



static void recordFragmentAttrsNotUpdated(KjNode* targetP, KjNode* fragP,
                                          const char* reason, const char* regId)
{
  if (fragP == NULL) return;
  for (KjNode* c = fragP->value.firstChildP; c != NULL; c = c->next)
  {
    if (isEntityKeyword(c->name)) continue;
    addNotUpdated(targetP, shortNameOf(c->name), reason, regId);
  }
}

static void recordFragmentAttrsUpdated(KjNode* targetP, KjNode* fragP)
{
  if (fragP == NULL) return;
  for (KjNode* c = fragP->value.firstChildP; c != NULL; c = c->next)
  {
    if (isEntityKeyword(c->name)) continue;
    addUpdatedUnique(targetP, shortNameOf(c->name));
  }
}



// -----------------------------------------------------------------------------
//
// patchEntityAttrs -
//
bool patchEntityAttrs(void)
{
  const char* entityId = swRest.in.wildcard[0];
  KjNode*     fragment = swRest.in.requestTree;

  //
  // Validate — LdOpUpdateEntity allows null-markers (unlike Create/Append).
  //
  if (ldCheckEntity(fragment, LdOpUpdateEntity, NULL, &swRest.kalloc) == false)
    return true;

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  KjNode* updatedP    = kjArray(swRest.kjsonP, "updated");
  KjNode* notUpdatedP = kjArray(swRest.kjsonP, "notUpdated");

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

  bool dispatch = (swNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;

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
        if (csr->endpoint == NULL)                    continue;
        if (ldDistOpCsrWouldLoop(csr, ownAlias))      continue;

        bool opSupported = ldRegOpSupported(csr, swRest.serviceP->ldOp);

        for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
        {
          if (!entityInfoCoversId(riP, entityId))
            continue;

          KjNode* fragP = ldEntityFragmentForInfo(fragment, riP, swRest.kjsonP, detach[g]);
          if (fragP == NULL)
            continue;

          if (!opSupported)
          {
            if (!opConf[g])
              continue;
            char reason[256];
            snprintf(reason, sizeof(reason),
                     "%s registration does not support updateAttrs", modeTag[g]);
            recordFragmentAttrsNotUpdated(notUpdatedP, fragP, reason, csr->regId);
            continue;
          }

          char*       body   = renderFragmentWithContext(fragP);
          const char* upErr  = NULL;
          int         upCode = ldDistOpSend(csr, SwVerbPatch,
                                            attrsUrl(csr->endpoint, entityId),
                                            body, strlen(body), ownAlias, &upErr);

          if (upCode >= 200 && upCode < 300)
          {
            anyCsrSucceeded = true;
            recordFragmentAttrsUpdated(updatedP, fragP);
          }
          else if (upCode != 404)
          {
            char reason[256];
            snprintf(reason, sizeof(reason), "%s",
                     ldDistOpForwardFailureReason(upCode, upErr));
            recordFragmentAttrsNotUpdated(notUpdatedP, fragP, reason, csr->regId);
          }
        }
      }
    }

    if (exclV  != NULL) free(exclV);
    if (redirV != NULL) free(redirV);
    if (inclV  != NULL) free(inclV);
  }

  //
  // Local path.
  //
  bool localHasAttrs = false;
  for (KjNode* c = fragment->value.firstChildP; c != NULL; c = c->next)
    if (!isEntityKeyword(c->name)) { localHasAttrs = true; break; }

  KjNode* existing = NULL;
  int     rr       = DB_NOT_FOUND;
  if (localHasAttrs || !anyCsrSucceeded)
    rr = db.entityRetrieve(tenantP, entityId, &existing);

  if (rr != DB_OK && !anyCsrSucceeded)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity '%s' not found", entityId);
    return true;
  }

  if (localHasAttrs && rr == DB_OK && existing != NULL)
  {
    //
    // § 5.6.2.4: if the fragment carries scope but the target entity has
    // no scope, the fragment's scope must be ignored (unlike Append where
    // it replaces). Strip it here so the downstream merge never sees it.
    //
    KjNode* fragScope = kjLookup(fragment, LD_VOCAB_SCOPE);
    if (fragScope != NULL && kjLookup(existing, LD_VOCAB_SCOPE) == NULL)
      kjChildRemove(fragment, fragScope);

    //
    // Record every non-keyword attr in the fragment into updated[] — spec
    // output says "List of Attributes actually updated". Null-markers
    // count as updates too (delete is a kind of update).
    //
    for (KjNode* c = fragment->value.firstChildP; c != NULL; c = c->next)
    {
      if (isEntityKeyword(c->name)) continue;
      addUpdatedUnique(updatedP, shortNameOf(c->name));
    }

    ldApiEntityToDbModel(fragment, &swRest.kalloc);

    if (db.entityAttrsSet == NULL)
    {
      ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented",
              "Update Attributes not supported by this DB plugin");
      return true;
    }

    // Update Attributes: scope semantics per § 5.6.2.4 are "replace"
    // (matching the default "overwrite allowed" since there's no
    // noOverwrite flag for this op).
    LdMergeReport report = { NULL };
    int r = db.entityAttrsSet(tenantP, entityId, fragment, true,
                               swRest.requestStartTime, &report);

    if (r != DB_OK && r != DB_NOT_FOUND)
    {
      ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
              "database error updating entity '%s'", entityId);
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

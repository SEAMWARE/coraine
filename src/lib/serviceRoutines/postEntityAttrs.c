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

#include "swNgsild/LdRegCache.h"                      // LdRegCache, LdRegCacheItem, LdRegMode, LdRegInfo
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "swNgsild/ldDistOp.h"                        // ldDistOpLoopDetected, ldDistOpSend, ldDistOpCsrWouldLoop
#include "swNgsild/ldEntityFragment.h"                // ldEntityFragmentForInfo

#include "db/DbDriver.h"                              // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                                // Tenant

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
// addNotUpdated - push a NotUpdatedDetails entry (§ 5.2.19)
//
static void addNotUpdated(KjNode* arrP, const char* attrName,
                          const char* reason, const char* regId)
{
  KjNode* entry = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(entry, kjString(swRest.kjsonP, "attributeName", attrName));
  kjChildAdd(entry, kjString(swRest.kjsonP, "reason",         reason));
  if (regId != NULL)
    kjChildAdd(entry, kjString(swRest.kjsonP, "registrationId", regId));
  kjChildAdd(arrP, entry);
}



// -----------------------------------------------------------------------------
//
// addUpdatedUnique - push an attr name to updated[] if not already present
//
static void addUpdatedUnique(KjNode* arrP, const char* attrName)
{
  for (KjNode* p = arrP->value.firstChildP; p != NULL; p = p->next)
    if (p->type == KjString && strcmp(p->value.s, attrName) == 0)
      return;
  kjChildAdd(arrP, kjString(swRest.kjsonP, NULL, attrName));
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
  const char* suffix  = "/attrs";
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
// recordFragmentAttrs - push every non-keyword attr in fragP onto targetP
//
// For CSR-failure reporting: every claimed attr in the slice goes into
// notUpdated[] with the supplied reason + registrationId, or into
// updated[] when the forward succeeded.
//
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
      addNotUpdated(notUpdatedP, shortName, "Attribute already exists", NULL);

    if (!anyKept)
      kjChildRemove(fragment, fAttrP);
    else
      addUpdatedUnique(updatedP, shortName);

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

  if (swRest.in.payload != NULL && fragment == NULL)
  {
    ldError(415, LD_ERROR_INVALID_REQUEST, "Unsupported Media Type",
            "supported Content-Types: application/json, application/ld+json");
    return true;
  }

  if (fragment == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "no payload");
    return true;
  }

  if (ldCheckEntity(fragment, LdOpAppendAttrs, NULL, &swRest.kalloc) == false)
    return true;

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;
  bool    noOverwrite = swNgsild.noOverwrite;

  //
  // DistOps dispatch (§ 5.6.3.4). Skipped on ?local=true, no regCache,
  // or Via loop. exclusive/redirect chop, inclusive clones.
  //
  // Fragment is in API form. ldEntityFragmentForInfo extracts per-CSR
  // slices (whose attrs match the CSR's propertyNames/relationshipNames)
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
              continue;  // inclusive silent-skip
            char reason[256];
            snprintf(reason, sizeof(reason),
                     "%s registration does not support appendAttrs", modeTag[g]);
            recordFragmentAttrsNotUpdated(notUpdatedP, fragP, reason, csr->regId);
            continue;
          }

          // Render + forward
          char*       body   = renderFragmentWithContext(fragP);
          const char* upErr  = NULL;
          int         upCode = ldDistOpSend(csr, SwVerbPost,
                                            attrsUrl(csr->endpoint, entityId, noOverwrite),
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
          // upCode == 404 tolerated silently
        }
      }
    }

    if (exclV  != NULL) free(exclV);
    if (redirV != NULL) free(redirV);
    if (inclV  != NULL) free(inclV);
  }

  //
  // Local path — what's left of the fragment (exclusive/redirect chop
  // removed their claims). If the fragment still has attrs, try local;
  // otherwise skip.
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
    // Entity unknown locally AND no CSR landed → 404 per § 5.6.3.4.
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity '%s' not found", entityId);
    return true;
  }

  if (localHasAttrs && rr == DB_OK && existing != NULL)
  {
    ldApiEntityToDbModel(fragment, &swRest.kalloc);
    classifyAndChopLocal(fragment, existing, noOverwrite, updatedP, notUpdatedP);

    if (db.entityAttrsSet == NULL)
    {
      ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented",
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

    if (r == DB_OK && tenantP->subCacheP != NULL)
    {
      KjNode* mergedEntity = NULL;
      db.entityRetrieve(tenantP, entityId, &mergedEntity);
      if (mergedEntity != NULL)
        ldNotifyDefer((LdSubCache*) tenantP->subCacheP, mergedEntity, LdNotifyEntityUpdate, &report);
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

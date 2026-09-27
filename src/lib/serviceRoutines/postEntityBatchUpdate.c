//
// FILE            postEntityBatchUpdate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// POST /ngsi-ld/v1/entityOperations/update — Batch Entity Update (§ 5.6.8).
//
// Semantics match Update Attributes (§ 5.6.3) per-entity, extended to a
// list. Multiple instances of the same entity id in one batch are
// **combined in array order** — first element is oldest, last is
// newest — so the merge sequence is deterministic even though every
// instance shares the same wall-clock modifiedAt. Notifications for
// intermediate merged states are queued via ldNotifyDefer and emitted
// post-response as one Notification per subscription with data[]
// carrying every matched state in encounter order.
//
// Flow:
//
//   Pass 1 — validate + group by id (preserving array order within a group).
//
//   Pass 2 — per group: retrieve existing; for each fragment in array
//            order, chop attrs claimed by exclusive/redirect CSRs into
//            per-CSR forwarding queues, clone (non-detaching) for
//            inclusive CSRs, then merge what remains into the in-memory
//            state. Defer one notification candidate per merged state.
//
//   Pass 3 — synchronous distops forward — for each accumulating CSR,
//            one POST /entityOperations/update carrying the ordered
//            array of its chopped/cloned fragments. URL is unchanged
//            from the incoming request (minimal-changes rule). Forward
//            failures populate errors[] with the CSR's registrationId.
//
//   Pass 4 — bulk DB write of the final merged states via
//            db.entityBulkUpdate.
//
//   Pass 5 — response: BatchOperationResult (§ 5.2.17). 204 all-OK,
//            207 partial, 409 all-failed.
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp, strlen, strncpy, strcasecmp, strcpy
#include <strings.h>                                 // strcasecmp
#include <stdlib.h>                                  // free
#include <stdio.h>                                   // snprintf

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/CorRestVerb.h"                       // CorVerbPost

#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeBuilder.h"                  // corTreeObject, corTreeArray, corTreeString, corTreeChildAdd, corTreeChildRemove
#include "corTree/corTreeClone.h"                    // corTreeClone
#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corJson/corJsonParse.h"                    // corJsonParse
#include "corJson/corJsonRender.h"                   // corJsonFastRender
#include "corJson/corJsonRenderSize.h"               // corJsonFastRenderSize

#include "corJsonld/corLdInit.h"                       // CORLD_CORE_CONTEXT_URL
#include "corJsonld/corLdDownload.h"                   // corLdContextFromUrl
#include "corJsonld/corLdCompactTree.h"                // corLdCompactTreeWith

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdOp.h"                           // LdOpUpdateAttrs, LdOpBatchUpdate
#include "corNgsild/LdNormalizeInput.h"               // ldNormalizeInput
#include "corNgsild/ldCheckEntity.h"                  // ldCheckEntity
#include "corNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "corNgsild/ldStripAtContext.h"              // ldStripAtContext
#include "corNgsild/LdProblem.h"                      // LD_ERROR_RESOURCE_NOT_FOUND, LD_ERROR_CONFLICT, LD_ERROR_INTERNAL_ERROR
#include "corNgsild/ldEntityAttrsSet.h"               // ldEntityAttrsSet
#include "corNgsild/ldEntityMerge.h"                  // LdMergeReport
#include "corNgsild/LdVocab.h"                        // LD_VOCAB_SCOPE
#include "corNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityUpdate
#include "corNgsild/ldNotifyDefer.h"                  // ldNotifyDefer
#include "bridge/channelCache.h"                      // channelOutCount
#include "bridge/bridgeServiceSync.h"             // bridgeRequestsBeforeWrite, bridgeRequestsWritten, BridgeSyncDone

#include "troe/troeFromMerge.h"                      // troeDeferAttrEventsFromMerge
#include "corNgsild/LdSubCache.h"                     // LdSubCache

#include "corNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode
#include "corNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "corNgsild/ldDistOp.h"                       // ldDistOp*
#include "corNgsild/ldEntityFragment.h"               // ldEntityFragmentForInfo
#include "corNgsild/ldIsEntityKeyword.h"                   // ldIsNotAttributeName

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND, DB_ERR
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postEntityBatchUpdate.h"   // Own interface



// -----------------------------------------------------------------------------
//
// noOverwriteChopLocal - strip from `fragment` every attribute that already
// exists on `existing` (matching by attr name and by instance datasetId).
// Returns the number of attribute conflicts (a "skip" count) — when > 0,
// the merge had nothing to do at attribute granularity.
//
// Mirror of postEntityAttrs::classifyAndChopLocal, minus the updated[] /
// notUpdated[] reporting (the batch response is a BatchOperationResult,
// not an UpdateResult — we only need the skip-count to drive 207-vs-204).
//
static int noOverwriteChopLocal(CorNode* fragment, CorNode* existing)
{
  if (fragment == NULL || existing == NULL)
    return 0;

  int skipped = 0;
  CorNode* fAttrP = fragment->value.firstChildP;
  while (fAttrP != NULL)
  {
    CorNode* nextAttr = fAttrP->next;
    if (ldIsNotAttributeName(fAttrP->name) || fAttrP->type != CorObject)
    {
      fAttrP = nextAttr;
      continue;
    }

    CorNode* tAttrP   = corTreeLookup(existing, fAttrP->name);
    bool    anyConflict = false;
    bool    anyKept     = false;

    if (tAttrP != NULL)
    {
      CorNode* fInstP = fAttrP->value.firstChildP;
      while (fInstP != NULL)
      {
        CorNode* nextInst = fInstP->next;
        if (fInstP->type == CorObject)
        {
          CorNode* tInstP = corTreeLookup(tAttrP, fInstP->name);
          if (tInstP != NULL)
          {
            anyConflict = true;
            corTreeChildRemove(fAttrP, fInstP);
            fInstP = nextInst;
            continue;
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

    if (anyConflict)
      skipped++;

    if (!anyKept)
      corTreeChildRemove(fragment, fAttrP);

    fAttrP = nextAttr;
  }
  return skipped;
}


// -----------------------------------------------------------------------------
//
// addBatchError - append a BatchEntityError (§ 5.2.17) to errors[].
//
static void addBatchError(CorNode* errorsP, const char* entityId, int statusCode,
                          const char* errType, const char* title,
                          const char* detail, const char* regId)
{
  CorNode* err = corTreeObject(corRest.kallocP, NULL);
  corTreeChildAdd(err, corTreeString(corRest.kallocP, "entityId", (char*) entityId));

  CorNode* pd = corTreeObject(corRest.kallocP, "error");
  corTreeChildAdd(pd, corTreeString (corRest.kallocP, "type", (char*) errType));
  corTreeChildAdd(pd, corTreeString (corRest.kallocP, "title", (char*) title));
  corTreeChildAdd(pd, corTreeInteger(corRest.kallocP, "status", statusCode));
  corTreeChildAdd(pd, corTreeString (corRest.kallocP, "detail", (char*) detail));
  corTreeChildAdd(err, pd);

  if (regId != NULL)
    corTreeChildAdd(err, corTreeString(corRest.kallocP, "registrationId", (char*) regId));

  corTreeChildAdd(errorsP, err);
}


// -----------------------------------------------------------------------------
//
// renderBatchBody - serialise the CorArray of fragments for forwarding.
//
// Two modes per § 4.3.6.6 + § 6.3.19:
//
//   - CSR has no "jsonldContext": per-element @context (core context URL),
//     application/ld+json.
//
//   - CSR has "jsonldContext": compact against that context + strip every
//     in-body @context; Content-Type application/json; Link header (set
//     by the dist-op layer) carries the URL.
//
static char* renderBatchBody(LdRegCacheItem* csr, CorNode* batchArr)
{
  //
  // § 4.3.6.6: compact every fragment against the effective forward
  // context — csi.jsonldContext > incoming request @context > core
  // (ldDistOpForwardContext: the same context buildHeaders names in the
  // Link header) — and strip in-body @context (the forward goes out as
  // application/json + Link).
  //
  CorLdContext* fwdCtx = ldDistOpForwardContext(csr);

  for (CorNode* fragP = batchArr->value.firstChildP; fragP != NULL; fragP = fragP->next)
  {
    corLdCompactTreeWith(fragP, fwdCtx);

    CorNode* atCtx = corTreeLookup(fragP, "@context");
    if (atCtx != NULL)
      corTreeChildRemove(fragP, atCtx);
  }

  int   bufSize = corJsonFastRenderSize(batchArr) + 1;
  char* buf     = (char*) kaAlloc(&corRest.kalloc, bufSize);
  corJsonFastRender(batchArr, buf);
  return buf;
}


// -----------------------------------------------------------------------------
//
// applyRemoteBatchResult - map the remote broker's BatchOperationResult
// to per-id outcomes. 2xx with no body means all forwarded ids OK; 207 with
// body → parse success[] and errors[]; anything else → one Bad-Gateway per
// forwarded id.
//
static void applyRemoteBatchResult(int status, CorNode* respTreeP,
                                    const char* csrRegId,
                                    CorNode* errorsP,
                                    bool* anyOkV,
                                    const char** idV, int N)
{
  bool success2xx = (status >= 200 && status < 300);

  if (success2xx && respTreeP == NULL)
  {
    for (int i = 0; i < N; i++) anyOkV[i] = true;
    return;
  }

  if (!success2xx || respTreeP == NULL)
  {
    char detail[256];
    snprintf(detail, sizeof(detail), "forward to '%s' failed (status %d)",
             csrRegId ? csrRegId : "?", status);
    for (int i = 0; i < N; i++)
      addBatchError(errorsP, idV[i], (status >= 400) ? status : 502,
                    LD_ERROR_INTERNAL_ERROR, "Bad Gateway", detail, csrRegId);
    return;
  }

  CorNode* remoteSuccess = corTreeLookup(respTreeP, "success");
  CorNode* remoteErrors = corTreeLookup(respTreeP, "errors");

  if (remoteSuccess != NULL && remoteSuccess->type == CorArray)
  {
    for (CorNode* sP = remoteSuccess->value.firstChildP; sP != NULL; sP = sP->next)
    {
      if (sP->type != CorString) continue;
      for (int i = 0; i < N; i++)
        if (strcmp(idV[i], sP->value.s) == 0) { anyOkV[i] = true; break; }
    }
  }

  if (remoteErrors != NULL && remoteErrors->type == CorArray)
  {
    for (CorNode* eP = remoteErrors->value.firstChildP; eP != NULL; eP = eP->next)
    {
      CorNode* idP    = corTreeLookup(eP, "entityId");
      CorNode* errP   = corTreeLookup(eP, "error");
      const char* eid = (idP != NULL && idP->type == CorString) ? idP->value.s : "";

      const char* type   = LD_ERROR_INTERNAL_ERROR;
      const char* title  = "Bad Gateway";
      const char* detail = "forward error";
      int         status = 502;
      if (errP != NULL && errP->type == CorObject)
      {
        CorNode* tP = corTreeLookup(errP, "type");
        CorNode* hP = corTreeLookup(errP, "title");
        CorNode* dP = corTreeLookup(errP, "detail");
        CorNode* sP = corTreeLookup(errP, "status");
        if (tP != NULL && tP->type == CorString) type   = tP->value.s;
        if (hP != NULL && hP->type == CorString) title  = hP->value.s;
        if (dP != NULL && dP->type == CorString) detail = dP->value.s;
        if (sP != NULL && sP->type == CorInt)    status = sP->value.i;
      }
      addBatchError(errorsP, eid, status, type, title, detail, csrRegId);
    }
  }
}


// -----------------------------------------------------------------------------
//
// Group - fragments targeting one entity id, in arrival order.
//
typedef struct Group
{
  const char*  id;
  CorNode**    fragV;
  int          count;
  int          capacity;
} Group;


// -----------------------------------------------------------------------------
//
// CsrAccum - one entry per CSR that accumulates forwardable fragments.
//
typedef struct CsrAccum
{
  LdRegCacheItem* csr;
  LdRegMode       mode;       // first mode that populated this accum — drives error behaviour
  CorNode**       fragV;      // chopped/cloned fragments, in encounter order
  const char**    idV;        // entity id per fragment (for error reporting)
  int             count;
  int             capacity;
} CsrAccum;


static Group* groupFindOrCreate(Group** groupsP, int* gNp, int* gCapP, const char* id)
{
  for (int i = 0; i < *gNp; i++)
    if (strcmp((*groupsP)[i].id, id) == 0)
      return &(*groupsP)[i];

  if (*gNp >= *gCapP)
  {
    int newCap = (*gCapP == 0) ? 8 : *gCapP * 2;
    Group* newV = (Group*) kaAlloc(&corRest.kalloc, newCap * sizeof(Group));
    for (int i = 0; i < *gNp; i++) newV[i] = (*groupsP)[i];
    *groupsP = newV;
    *gCapP   = newCap;
  }

  (*groupsP)[*gNp].id       = id;
  (*groupsP)[*gNp].fragV    = NULL;
  (*groupsP)[*gNp].count    = 0;
  (*groupsP)[*gNp].capacity = 0;
  return &(*groupsP)[(*gNp)++];
}


static void groupFragAppend(Group* g, CorNode* fragP)
{
  if (g->count >= g->capacity)
  {
    int newCap = (g->capacity == 0) ? 4 : g->capacity * 2;
    CorNode** newV = (CorNode**) kaAlloc(&corRest.kalloc, newCap * sizeof(CorNode*));
    for (int i = 0; i < g->count; i++) newV[i] = g->fragV[i];
    g->fragV    = newV;
    g->capacity = newCap;
  }
  g->fragV[g->count++] = fragP;
}


static CsrAccum* csrAccumFindOrCreate(CsrAccum** aV, int* aN, int* aCap,
                                      LdRegCacheItem* csr, LdRegMode mode)
{
  for (int i = 0; i < *aN; i++)
    if ((*aV)[i].csr == csr)
      return &(*aV)[i];

  if (*aN >= *aCap)
  {
    int newCap = (*aCap == 0) ? 4 : *aCap * 2;
    CsrAccum* newV = (CsrAccum*) kaAlloc(&corRest.kalloc, newCap * sizeof(CsrAccum));
    for (int i = 0; i < *aN; i++) newV[i] = (*aV)[i];
    *aV   = newV;
    *aCap = newCap;
  }

  (*aV)[*aN].csr      = csr;
  (*aV)[*aN].mode     = mode;
  (*aV)[*aN].fragV    = NULL;
  (*aV)[*aN].idV      = NULL;
  (*aV)[*aN].count    = 0;
  (*aV)[*aN].capacity = 0;
  return &(*aV)[(*aN)++];
}


static void csrAccumAppend(CsrAccum* a, CorNode* fragP, const char* id)
{
  if (a->count >= a->capacity)
  {
    int newCap = (a->capacity == 0) ? 4 : a->capacity * 2;
    CorNode**    newF  = (CorNode**)    kaAlloc(&corRest.kalloc, newCap * sizeof(CorNode*));
    const char** newId = (const char**) kaAlloc(&corRest.kalloc, newCap * sizeof(char*));
    for (int i = 0; i < a->count; i++)
    {
      newF[i]  = a->fragV[i];
      newId[i] = a->idV[i];
    }
    a->fragV    = newF;
    a->idV      = newId;
    a->capacity = newCap;
  }
  a->fragV[a->count]  = fragP;
  a->idV[a->count]    = id;
  a->count++;
}


// -----------------------------------------------------------------------------
//
// chopForMode - walk every matching CSR of the given mode for (id, type, scope),
// chop or clone attrs from fragP into the per-CSR accumulator.
//
//   mode == exclusive or redirect → detach=true (attrs leave fragP)
//   mode == inclusive             → detach=false (clone; fragP keeps the attrs)
//
static void chopForMode(Tenant*      tenantP,
                         const char*  entityId,
                         char**       typeArr,
                         char**       scopeV,
                         CorNode*     fragP,
                         LdRegMode    mode,
                         bool         detach,
                         CsrAccum**   aVp,
                         int*         aNp,
                         int*         aCapP,
                         const char*  ownAlias)
{
  if (tenantP->regCacheP == NULL)
    return;

  LdRegCacheItem** matchV = NULL;
  int matchN = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                 entityId, typeArr, scopeV,
                                                 mode, &matchV);

  for (int m = 0; m < matchN; m++)
  {
    LdRegCacheItem* csr = matchV[m];
    if (csr->endpoint == NULL)                 continue;
    if (ldDistOpCsrWouldLoop(csr, ownAlias))   continue;

    for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
    {
      CorNode* chopped = ldEntityFragmentForInfo(fragP, riP, corRest.kallocP, detach);
      if (chopped == NULL)
        continue;

      CsrAccum* a = csrAccumFindOrCreate(aVp, aNp, aCapP, csr, mode);
      csrAccumAppend(a, chopped, entityId);
    }
  }

  ldRegCacheMatchRelease(matchV, matchN);
}


//
// purgeRedirAttrsFromFragment - strip from `fragP` every attribute
// that any redirect-matched CSR claims, AFTER the redirect chopForMode
// call has run with detach=false. We have to defer the detach until all
// redirect CSRs covering this entity have been served, otherwise the
// second-and-onwards ones find an empty fragment (D008_01_red-class
// bug; surfaced here by ETSI D013_01/02_red).
//
static void purgeRedirAttrsFromFragment(Tenant*     tenantP,
                                         const char* entityId,
                                         char**      typeArr,
                                         char**      scopeV,
                                         CorNode*    fragP,
                                         const char* ownAlias)
{
  if (tenantP->regCacheP == NULL)
    return;

  LdRegCacheItem** matchV = NULL;
  int matchN = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                 entityId, typeArr, scopeV,
                                                 LdRegModeRedirect, &matchV);

  for (int m = 0; m < matchN; m++)
  {
    LdRegCacheItem* csr = matchV[m];
    if (csr->endpoint == NULL)                 continue;
    if (ldDistOpCsrWouldLoop(csr, ownAlias))   continue;

    for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
      (void) ldEntityFragmentForInfo(fragP, riP, corRest.kallocP, /*detach=*/true);
  }

  ldRegCacheMatchRelease(matchV, matchN);
}


//
// anyCsrMatchesEntity - true if any exclusive/redirect/inclusive CSR
// matches the entity (id + type). § 10.x: a batch-update target unknown
// locally is ResourceNotFound only when "no matching registrations
// apply" (§ 9.4) — with a match, the update still forwards and only the
// local merge leg is skipped.
//
static bool anyCsrMatchesEntity(Tenant* tenantP, const char* entityId, CorNode* firstFragP)
{
  if (tenantP->regCacheP == NULL)
    return false;

  CorNode* typeP     = (firstFragP != NULL) ? corTreeLookup(firstFragP, "type") : NULL;
  char*   typeArr[2] = { NULL, NULL };
  char**  typeArgP   = NULL;
  if (typeP != NULL && typeP->type == CorString)
  {
    typeArr[0] = typeP->value.s;
    typeArgP   = typeArr;
  }

  static const LdRegMode modes[] = { LdRegModeExclusive, LdRegModeRedirect, LdRegModeInclusive };
  for (int i = 0; i < 3; i++)
  {
    LdRegCacheItem** v = NULL;
    int n = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                              entityId, typeArgP, NULL, modes[i], &v);
    if (v != NULL) free(v);
    if (n > 0) return true;
  }
  return false;
}


// -----------------------------------------------------------------------------
//
// hasLocalPayload - true if fragP still carries something to merge
// locally (i.e. it's not just { id, @context } after chopping).
//
// `type` and `scope` count — they aren't "attributes" strictly but they
// trigger entity-level union/replace updates that the local store must
// see, otherwise a fragment of { id, type: NewType } is silently lost
// (ETSI 005_05_01: Batch Update used only to add an Entity Type).
//
static bool hasLocalPayload(CorNode* fragP)
{
  if (fragP == NULL || fragP->type != CorObject) return false;
  for (CorNode* c = fragP->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->name == NULL)                 continue;
    if (c->name[0] == '@')               continue;
    if (strcmp(c->name, "id")   == 0)    continue;
    return true;
  }
  return false;
}


// -----------------------------------------------------------------------------
//
// hasAttribute - is anything left in the fragment to merge?
//
static bool hasAttribute(CorNode* fragP)
{
  for (CorNode* c = fragP->value.firstChildP; c != NULL; c = c->next)
  {
    if ((c->type == CorObject) && (ldIsNotAttributeName(c->name) == false))
      return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// seenAdd - noOverwrite: the attributes and instances a fragment brings, for the later fragments of its Entity
//
// What noOverwriteChopLocal looks at in an Entity - an attribute, and its
// instances by datasetId - and nothing else: names only, empty objects. In
// one loop the merge put them in existingDb; with the merge in loop 2, a
// later fragment is chopped against this as well.
//
static CorNode* seenAdd(CorNode* seenP, CorNode* fragP)
{
  if (seenP == NULL)
    seenP = corTreeObject(corRest.kallocP, NULL);

  for (CorNode* attrP = fragP->value.firstChildP; attrP != NULL; attrP = attrP->next)
  {
    if ((attrP->type != CorObject) || (ldIsNotAttributeName(attrP->name) == true))
      continue;

    CorNode* seenAttrP = corTreeLookup(seenP, attrP->name);

    if (seenAttrP == NULL)
    {
      seenAttrP = corTreeObject(corRest.kallocP, attrP->name);
      corTreeChildAdd(seenP, seenAttrP);
    }

    for (CorNode* instP = attrP->value.firstChildP; instP != NULL; instP = instP->next)
    {
      if ((instP->type == CorObject) && (corTreeLookup(seenAttrP, instP->name) == NULL))
        corTreeChildAdd(seenAttrP, corTreeObject(corRest.kallocP, instP->name));
    }
  }

  return seenP;
}



// -----------------------------------------------------------------------------
//
// postEntityBatchUpdate -
//
bool postEntityBatchUpdate(void)
{
  CorNode* bodyP = corRest.in.requestTree;

  if (bodyP->type != CorArray)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Not a JSON Array",
            "Batch Entity Update body must be a JSON array");
    return true;
  }

  int total = 0;
  for (CorNode* c = bodyP->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->type == CorNull)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Array Entry",
              "Batch Entity Update: null entry at position %d", total);
      return true;
    }
    total++;
  }

  bool hasPreErrors = (corNgsild.batchPreErrors != NULL &&
                       corNgsild.batchPreErrors->value.firstChildP != NULL);
  if (total == 0 && !hasPreErrors)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Empty Array",
            "Batch Entity Update: input array is empty");
    return true;
  }

  CorNode* successP = corTreeArray(corRest.kallocP, "success");
  CorNode* errorsP = corTreeArray(corRest.kallocP, "errors");

  if (hasPreErrors)
  {
    errorsP->value.firstChildP = corNgsild.batchPreErrors->value.firstChildP;
    errorsP->lastChild         = corNgsild.batchPreErrors->lastChild;
    corNgsild.batchPreErrors    = NULL;
  }

  //
  // Pass 1 — validate, normalise, group by id.
  //
  Group* groups = NULL;
  int    gN     = 0;
  int    gCap   = 0;

  for (CorNode* inP = bodyP->value.firstChildP; inP != NULL; inP = inP->next)
  {
    if (inP->type != CorObject)
    {
      addBatchError(errorsP, "", 400,
                    LD_ERROR_BAD_REQUEST_DATA, "Invalid Array Entry",
                    "entity must be a JSON object", NULL);
      continue;
    }

    //
    // Normalization can REFUSE now - an attribute whose "type" names no NGSI-LD
    // Attribute type is an error, not something to infer a type for. It has
    // already set the problem, so it joins the existing check: short-circuit
    // means ldCheckEntity is skipped and the handling below reports it.
    //
    if (ldNormalizeInput(inP, &corRest.kalloc, false, false) == false ||
        ldCheckEntity(inP, LdOpUpdateAttrs, NULL, &corRest.kalloc) == false)
    {
      const char* eid = "";
      CorNode* idP = corTreeLookup(inP, "id");
      if (idP != NULL && idP->type == CorString) eid = idP->value.s;

      //
      // problemDetail is handed straight to addBatchError - no local copy. It
      // ends up in corTreeString(), which memcpy's the value into the CorNode's own
      // allocation, so the buffer is free to be reused on the next iteration
      // before this one is rendered.
      //
      // "Invalid Entity" is DELIBERATE, and decided here rather than taken from
      // corRest.out.problemTitle: an invalid attribute makes the whole entity
      // invalid, and the whole entity is what gets discarded. The specific title
      // the check function set is not lost - ldError logs it, with its own file
      // and line.
      //
      addBatchError(errorsP, eid, 400,
                    LD_ERROR_BAD_REQUEST_DATA, "Invalid Entity", corRest.out.problemDetail, NULL);

      corRest.out.httpStatusCode   = 0;
      corRest.out.problemType      = NULL;
      corRest.out.problemTitle     = NULL;
      corRest.out.problemDetail[0] = 0;
      continue;
    }

    CorNode* idP = corTreeLookup(inP, "id");
    if (idP == NULL || idP->type != CorString)
    {
      addBatchError(errorsP, "", 400,
                    LD_ERROR_BAD_REQUEST_DATA, "Invalid Array Entry",
                    "entity id is missing or not a string", NULL);
      continue;
    }

    //
    // § 9.3.3 guard — a ?local=true write must not produce local data that
    // an exclusive or redirect registration claims.
    //
    if (corNgsild.local == true && ((Tenant*) corNgsild.tenantP)->regCacheP != NULL)
    {
      const char* cRegId = ldRegCacheLocalWriteConflictTree((LdRegCache*) ((Tenant*) corNgsild.tenantP)->regCacheP,
                                                            idP->value.s, inP, &corRest.kalloc);
      if (cRegId != NULL)
      {
        addBatchError(errorsP, idP->value.s, 409,
                      LD_ERROR_ALREADY_EXISTS, "Conflict",
                      "local write overlaps with registration (§ 9.3.3 — no local data for an exclusive/redirect scope)",
                      cRegId);
        continue;
      }
    }

    Group* g = groupFindOrCreate(&groups, &gN, &gCap, idP->value.s);
    groupFragAppend(g, inP);
  }

  if (gN == 0)
  {
    int singleStatus = ldBatchErrorsSingleStatus(errorsP);
    if (singleStatus > 0)
    {
      corRest.out.responseTree   = ldBatchErrorAsProblemDetails(errorsP);
      corRest.out.httpStatusCode = singleStatus;
    }
    else
    {
      CorNode* respBodyP = corTreeObject(corRest.kallocP, NULL);
      corTreeChildAdd(respBodyP, successP);
      corTreeChildAdd(respBodyP, errorsP);
      corRest.out.responseTree   = respBodyP;
      corRest.out.httpStatusCode = 207;
    }
    // rawResponse for BOTH branches — neither ProblemDetails nor
    // BatchOperationResult is an entity tree; ldEntityToApi must not
    // run on them.
    corNgsild.rawResponse = true;
    return true;
  }

  //
  // Pass 2 — per group: retrieve, per fragment chop + merge, defer notifs.
  //
  Tenant*      tenantP   = (Tenant*) corNgsild.tenantP;
  LdSubCache*  subCacheP = (LdSubCache*) tenantP->subCacheP;

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);

  bool dispatch = (corNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;

  CsrAccum*    csrAccums = NULL;
  int          csrAccumsN   = 0;
  int          csrAccumsCap = 0;

  CorNode*     finals   = corTreeArray(corRest.kallocP, NULL);
  const char** finalIdV = (const char**) kaAlloc(&corRest.kalloc, sizeof(char*) * gN);
  CorNode**    finalEntityV = (CorNode**) kaAlloc(&corRest.kalloc, sizeof(CorNode*) * gN);

  //
  // Requests to the DDS side go FIRST, per fragment, before the bulk write -
  // and what they sent is released once that write is done. Only when a bridge
  // carries anything: a batch of five hundred with no bridge allocates nothing.
  //
  bool             requestsFirst  = (channelOutCount() > 0);
  BridgeSyncDone** doneV     = NULL;
  int              doneN     = 0;

  if (requestsFirst == true)                              // nothing at all without a bridge - not even the count
  {
    int fragN = 0;

    for (int gi = 0; gi < gN; gi++)
      fragN += groups[gi].count;

    doneV = (BridgeSyncDone**) kaAlloc(&corRest.kalloc, sizeof(BridgeSyncDone*) * (fragN + 1));
  }
  bool*        anySuccessV = (bool*) kaAlloc(&corRest.kalloc, sizeof(bool) * gN);
  const char** allIdV   = (const char**) kaAlloc(&corRest.kalloc, sizeof(char*) * gN);
  int          finalN   = 0;

  //
  // ⭐ TWO LOOPS, split where the DDS step waits. Loop 1 chops each fragment and
  // SENDS its requests to the DDS side; every goal of the batch is then waited
  // for against one deadline; loop 2 merges what the transport accepted. In
  // one loop, each Entity's goals would wait before the next Entity's were even
  // sent - a batch's wait the sum of its goals', not the slowest of them.
  //
  // What crosses from one loop to the other is per Entity (a group) or per
  // fragment, below. Errors are collected per Entity and joined in batch order
  // at the end, so the split does not reorder errors[].
  //
  int fragTotal = 0;

  for (int gi = 0; gi < gN; gi++)
    fragTotal += groups[gi].count;

  CorNode**        existingDbV      = (CorNode**)        kaAlloc(&corRest.kalloc, sizeof(CorNode*) * gN);
  CorNode**        groupErrorsV     = (CorNode**)        kaAlloc(&corRest.kalloc, sizeof(CorNode*) * gN);
  bool*            groupLiveV       = (bool*)            kaAlloc(&corRest.kalloc, sizeof(bool)    * gN);   // got past the retrieve
  bool*            noOverwriteSkipV = (bool*)            kaAlloc(&corRest.kalloc, sizeof(bool)    * gN);
  int*             fragBaseV        = (int*)             kaAlloc(&corRest.kalloc, sizeof(int)     * gN);   // a group's first fragment, in the per-fragment arrays
  bool*            readyV           = (bool*)            kaAlloc(&corRest.kalloc, sizeof(bool)    * (fragTotal + 1));   // made it through loop 1
  BridgeSyncDone** fragDoneV        = (requestsFirst == true) ? (BridgeSyncDone**) kaAlloc(&corRest.kalloc, sizeof(BridgeSyncDone*) * (fragTotal + 1)) : NULL;

  memset(groupErrorsV, 0, sizeof(CorNode*) * gN);
  memset(groupLiveV,   0, sizeof(bool)    * gN);
  memset(readyV,       0, sizeof(bool)    * (fragTotal + 1));

  if (fragDoneV != NULL)
    memset(fragDoneV, 0, sizeof(BridgeSyncDone*) * (fragTotal + 1));

  for (int gi = 0, base = 0; gi < gN; gi++)
  {
    fragBaseV[gi]        = base;
    noOverwriteSkipV[gi] = false;
    base                += groups[gi].count;
  }

  //
  // Loop 1 - chop, and send.
  //
  for (int gi = 0; gi < gN; gi++)
  {
    Group*  g            = &groups[gi];
    CorNode* groupErrorsP = corTreeArray(corRest.kallocP, NULL);

    groupErrorsV[gi] = groupErrorsP;
    allIdV[gi]     = g->id;
    anySuccessV[gi] = false;

    CorNode* existingDb = NULL;
    if (db.entityRetrieve == NULL)
    {
      addBatchError(groupErrorsP, g->id, 500,
                    LD_ERROR_INTERNAL_ERROR, "Internal Error",
                    "entityRetrieve not supported by this DB plugin", NULL);
      continue;
    }

    int r = db.entityRetrieve(tenantP, g->id, &existingDb);
    if (r == DB_NOT_FOUND || existingDb == NULL)
    {
      // Not local — still forwardable when a registration matches
      // (§ 9.4); only "unknown locally AND no matching registrations"
      // is ResourceNotFound (ETSI D014_*: the entity lives behind a
      // redirect CSR and the update must reach it).
      existingDb = NULL;
      if (!dispatch || !anyCsrMatchesEntity(tenantP, g->id, (g->count > 0) ? g->fragV[0] : NULL))
      {
        addBatchError(groupErrorsP, g->id, 404,
                      LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                      "entity does not exist", NULL);
        continue;
      }
    }
    else if (r != DB_OK)
    {
      addBatchError(groupErrorsP, g->id, 500,
                    LD_ERROR_INTERNAL_ERROR, "Internal Error",
                    "database error during retrieve", NULL);
      continue;
    }

    groupLiveV[gi]  = true;
    existingDbV[gi] = existingDb;

    //
    // Each fragment in array order: distops chop, then - the fragment being
    // final now - its requests to the DDS side are SENT. Merged in loop 2.
    //
    bool    anyNoOverwriteSkip = false;
    CorNode* seenP             = NULL;                // noOverwrite: what the earlier fragments bring
    for (int fi = 0; fi < g->count; fi++)
    {
      CorNode* fragP = g->fragV[fi];

      //
      // Count the fragment's "real" attrs (Object children excluding
      // entity keywords) BEFORE chopping. If the fragment originally
      // carried attrs but distops chops all of them, the local leg has
      // nothing to do — even though `type`/`scope`/etc. may still be
      // present, those don't count as a successful update on their own
      // when the user actually asked to update attributes that all got
      // forwarded. Without this guard the entity is silently reported
      // as a local success (anyMerge=true → bulkUpdate → success[]) on
      // top of the per-CSR error, which lifts a single-failure batch
      // to 207 instead of the expected 409.
      //
      int realAttrsBefore = 0;
      for (CorNode* c = fragP->value.firstChildP; c != NULL; c = c->next)
      {
        if (c->type != CorObject) continue;
        if (ldIsNotAttributeName(c->name)) continue;
        realAttrsBefore++;
      }

      //
      // Distops chop (order matters: exclusive first, then redirect, then
      // inclusive — see § 4.3.6.3).
      //
      if (dispatch)
      {
        CorNode* typeP = corTreeLookup(fragP, "type");
        char*   typeArr[2] = { NULL, NULL };
        if (typeP != NULL && typeP->type == CorString)
          typeArr[0] = typeP->value.s;

        CorNode* scopeP      = corTreeLookup(fragP, "scope");
        char*   scopeBuf[2]  = { NULL, NULL };
        char**  scopeV       = NULL;
        if (scopeP != NULL && scopeP->type == CorString)
        {
          scopeBuf[0] = scopeP->value.s;
          scopeV      = scopeBuf;
        }

        chopForMode(tenantP, g->id, typeArr, scopeV, fragP,
                    LdRegModeExclusive, true,
                    &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
        // Redirect: clone (multiple redirect CSRs may cover the same
        // entity, all must receive a copy), then purge the redirect-
        // claimed attrs once, so the local apply only sees what's left.
        chopForMode(tenantP, g->id, typeArr, scopeV, fragP,
                    LdRegModeRedirect, false,
                    &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
        purgeRedirAttrsFromFragment(tenantP, g->id, typeArr, scopeV, fragP, ownAlias);
        chopForMode(tenantP, g->id, typeArr, scopeV, fragP,
                    LdRegModeInclusive, false,
                    &csrAccums, &csrAccumsN, &csrAccumsCap, ownAlias);
      }

      //
      // Local merge — only if there are attrs left after chopping. The
      // check runs on API-form fragP before ldApiEntityToDbModel injects
      // createdAt/modifiedAt, so a fully-chopped fragment is correctly
      // seen as attr-less.
      //
      if (!hasLocalPayload(fragP))
        continue;

      //
      // Type-only fragments (5.5.5) are legitimately mergeable with no
      // attribute payload — let them through. But when the user did
      // supply attrs and distops chopped them all, we have nothing local
      // left to write.
      //
      if (realAttrsBefore > 0)
      {
        int realAttrsAfter = 0;
        for (CorNode* c = fragP->value.firstChildP; c != NULL; c = c->next)
        {
          if (c->type != CorObject) continue;
          if (ldIsNotAttributeName(c->name)) continue;
          realAttrsAfter++;
        }
        if (realAttrsAfter == 0)
          continue;
      }

      // Entity not local (forward-only target): the attrs that survived
      // the chop have no local entity to land on.
      if (existingDb == NULL)
      {
        addBatchError(groupErrorsP, g->id, 404,
                      LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                      "entity does not exist locally; attributes not covered by any registration", NULL);
        continue;
      }

      ldApiEntityToDbModel(fragP, &corRest.kalloc, 0);

      //
      // ?options=noOverwrite — strip any attrs already present on the
      // entity. Per § 5.6.18 Update Batch, "skipped" attrs aren't an
      // entity-level error, but if EVERY attr was skipped the entity
      // had nothing to update and the batch reports 207 with an entry
      // in errors[] (so the client can see which entities were no-ops).
      //
      // Runs AFTER ldApiEntityToDbModel so both fragP and existingDb
      // share the DB shape (expanded IRIs + @none dataset wrappers).
      //
      if (corNgsild.noOverwrite)
      {
        //
        // Against what is stored AND what this batch's earlier fragments of
        // the same Entity bring - the merge that would have added those to
        // existingDb comes only in the second loop (see "Two loops").
        //
        if (noOverwriteChopLocal(fragP, existingDb) + noOverwriteChopLocal(fragP, seenP) > 0)
          anyNoOverwriteSkip = true;

        // Re-check whether any attribute (not just id/type/timestamps)
        // remains. hasLocalPayload returns true on `type`/`createdAt`/
        // `modifiedAt` alone, which would still proceed to a no-op merge.
        bool anyAttrLeft = false;
        for (CorNode* c = fragP->value.firstChildP; c != NULL; c = c->next)
        {
          if (c->type != CorObject) continue;
          if (ldIsNotAttributeName(c->name)) continue;
          anyAttrLeft = true;
          break;
        }
        if (!anyAttrLeft)
          continue;
      }

      readyV[fragBaseV[gi] + fi] = true;

      if (corNgsild.noOverwrite)
        seenP = seenAdd(seenP, fragP);

      if (requestsFirst == true)
      {
        BridgeSyncDone* doneP = (BridgeSyncDone*) kaAlloc(&corRest.kalloc, sizeof(BridgeSyncDone));

        memset(doneP, 0, sizeof(BridgeSyncDone));
        doneV[doneN++]                = doneP;
        fragDoneV[fragBaseV[gi] + fi] = doneP;

        bridgeRequestsBeforeWrite(tenantP, g->id, fragP, BRIDGE_REQ_PER_ENTITY | BRIDGE_REQ_SEND_ONLY, doneP);
      }
    }

    noOverwriteSkipV[gi] = anyNoOverwriteSkip;
  }

  //
  // Every goal of the batch is out - now they are waited for, against ONE
  // deadline, so that a goal's answer never waits for the next goal to be
  // sent. A refused one is taken out of its fragment here.
  //
  if (requestsFirst == true)
  {
    int64_t dueMs = bridgeRequestsDeadline();

    for (int gi = 0; gi < gN; gi++)
    {
      for (int fi = 0; fi < groups[gi].count; fi++)
      {
        BridgeSyncDone* doneP = fragDoneV[fragBaseV[gi] + fi];

        if (doneP != NULL)
          bridgeRequestsAwait(groups[gi].fragV[fi], doneP, dueMs);
      }
    }
  }

  //
  // Loop 2 - each fragment the first loop made ready, in array order: what the
  // DDS step refused is reported, and the rest merged, notified and recorded.
  //
  for (int gi = 0; gi < gN; gi++)
  {
    if (groupLiveV[gi] == false)
      continue;

    Group*        g                  = &groups[gi];
    CorNode*      existingDb         = existingDbV[gi];
    CorNode*      groupErrorsP       = groupErrorsV[gi];
    bool          anyNoOverwriteSkip = noOverwriteSkipV[gi];
    bool          anyMerge           = false;

    for (int fi = 0; fi < g->count; fi++)
    {
      if (readyV[fragBaseV[gi] + fi] == false)
        continue;

      CorNode*        fragP = g->fragV[fi];
      BridgeSyncDone* doneP = (fragDoneV != NULL) ? fragDoneV[fragBaseV[gi] + fi] : NULL;

      //
      // A request to the DDS side that could not be sent, or a goal that was
      // refused, was left out of the fragment - this Entity's error. A fragment
      // that had nothing else to write is then not merged at all.
      //
      if (doneP != NULL)
      {
        for (int ix = 0; ix < doneP->failedN; ix++)
        {
          addBatchError(groupErrorsP, g->id, doneP->failedStatusV[ix],
                        doneP->failedTypeV[ix],
                        doneP->failedTitleV[ix],
                        doneP->failedReasonV[ix], NULL);
        }

        if ((doneP->failedN > 0) && (hasAttribute(fragP) == false))
          continue;
      }

      LdMergeReport report = { NULL };
      ldEntityAttrsSet(existingDb, fragP, true /* overwriteScope */,
                       corRest.requestStartTime, &report, corRest.kallocP);
      anyMerge = true;

      if (subCacheP != NULL)
      {
        CorNode* snapshot = corTreeClone(corRest.kallocP, existingDb);
        ldNotifyDefer(subCacheP, snapshot, LdNotifyEntityUpdate, &report);
      }

      // TRoE: per-fragment attr events. Optimistic — fires regardless
      // of the later bulk_update outcome (matches the notification
      // dispatch's pre-existing optimism).
      {
        CorNode* tn = corTreeLookup(existingDb, "type");
        const char* etype = (tn != NULL && tn->type == CorString) ? tn->value.s : NULL;
        troeDeferAttrEventsFromMerge(tenantP, g->id, etype, existingDb, &report,
                                     corRest.requestStartTime);
      }
    }

    if (anyMerge)
    {
      finalEntityV[finalN]  = existingDb;
      finalIdV[finalN++]    = g->id;
      corTreeChildAdd(finals, existingDb);

      // Mixed outcome: some attrs were merged, some were skipped by
      // noOverwrite. § 5.6.18: per-entity partial success is reported
      // in errors[] alongside the entity's id in success[], driving the
      // overall response to 207 instead of 204 (ETSI 005_02_03).
      if (anyNoOverwriteSkip)
        addBatchError(groupErrorsP, g->id, 400,
                      LD_ERROR_BAD_REQUEST_DATA, "Already Exists",
                      "some attrs already existed; skipped under noOverwrite",
                      NULL);
    }
    else if (anyNoOverwriteSkip)
    {
      // All attrs skipped by noOverwrite — entity had nothing to update.
      // Surface as a BatchEntityError so the response status is 207, not
      // a silent 204 (ETSI 005_02_01 expects this).
      addBatchError(groupErrorsP, g->id, 400,
                    LD_ERROR_BAD_REQUEST_DATA, "Already Exists",
                    "all attrs already exist; nothing to update under noOverwrite",
                    NULL);
    }
  }

  //
  // Each Entity's errors, in batch order - as one loop would have reported them.
  //
  for (int gi = 0; gi < gN; gi++)
  {
    CorNode* errP = (groupErrorsV[gi] != NULL) ? groupErrorsV[gi]->value.firstChildP : NULL;

    while (errP != NULL)
    {
      CorNode* nextP = errP->next;

      errP->next = NULL;                              // ⚠ corTreeChildAdd would take the rest of the list along
      corTreeChildAdd(errorsP, errP);
      errP = nextP;
    }
  }

  //
  // Pass 3 — concurrent distops forward, one POST /entityOperations/update
  // per accumulating CSR, all in flight at once via ldDistOpSendMulti.
  //
  {
    LdDistOpBatchItem*   bItems   = (LdDistOpBatchItem*)   kaAlloc(&corRest.kalloc, csrAccumsN * sizeof(LdDistOpBatchItem));
    memset(bItems, 0, csrAccumsN * sizeof(LdDistOpBatchItem));
    LdDistOpBatchResult* bResults = (LdDistOpBatchResult*) kaAlloc(&corRest.kalloc, csrAccumsN * sizeof(LdDistOpBatchResult));
    int                  bIdx[csrAccumsN];   // map batch index → csrAccums index
    int                  bCount   = 0;
    memset(bResults, 0, csrAccumsN * sizeof(LdDistOpBatchResult));

    // The forwarded leg must apply the same overwrite mode as the incoming
    // request — propagate ?options=noOverwrite (cf. batch upsert's
    // ?options=update propagation).
    const char* batchPath = (corNgsild.noOverwrite) ? "/ngsi-ld/v1/entityOperations/update?options=noOverwrite"
                                                   : "/ngsi-ld/v1/entityOperations/update";
    int         batchPathLen = strlen(batchPath);

    for (int ai = 0; ai < csrAccumsN; ai++)
    {
      CsrAccum* a   = &csrAccums[ai];
      LdRegCacheItem* csr = a->csr;

      if (a->count == 0) continue;

      if (!ldRegOpSupported(csr, LdOpBatchUpdate))
      {
        if (a->mode == LdRegModeExclusive || a->mode == LdRegModeRedirect)
        {
          const char* detail = (a->mode == LdRegModeExclusive)
                               ? "exclusive registration does not support updateBatch"
                               : "redirect registration does not support updateBatch";
          for (int i = 0; i < a->count; i++)
            addBatchError(errorsP, a->idV[i], 409,
                          LD_ERROR_CONFLICT, "Conflict", detail, csr->regId);
        }
        continue;
      }

      CorNode* batchArr = corTreeArray(corRest.kallocP, NULL);
      for (int i = 0; i < a->count; i++)
        corTreeChildAdd(batchArr, a->fragV[i]);

      int   baseLen = strlen(csr->endpoint);
      char* url     = (char*) kaAlloc(&corRest.kalloc, baseLen + batchPathLen + 1);
      strcpy(url, csr->endpoint);
      strcpy(url + baseLen, batchPath);

      char* body = renderBatchBody(csr, batchArr);

      bItems[bCount].csr     = csr;
      bItems[bCount].url     = url;
      bItems[bCount].body    = body;
      bItems[bCount].bodyLen = strlen(body);
      bIdx[bCount]           = ai;
      bCount++;
    }

    if (bCount > 0)
    {
      ldDistOpSendMulti(bItems, bCount, CorVerbPost, ownAlias, bResults);

      for (int bi = 0; bi < bCount; bi++)
      {
        CsrAccum* a = &csrAccums[bIdx[bi]];

        CorNode* respTreeP = NULL;
        if (bResults[bi].responseBody != NULL && bResults[bi].responseBodyLen > 0)
        {
          CorNode* treeP = bResults[bi].responseTree;
          if (treeP != NULL)
          {
            ldStripAtContext(treeP);
            respTreeP = treeP;
          }
        }

        bool* groupOk = (bool*) kaAlloc(&corRest.kalloc, sizeof(bool) * a->count);
        for (int k = 0; k < a->count; k++) groupOk[k] = false;

        applyRemoteBatchResult(bResults[bi].statusCode, respTreeP, bItems[bi].csr->regId,
                               errorsP, groupOk, a->idV, a->count);

        for (int k = 0; k < a->count; k++)
        {
          if (!groupOk[k]) continue;
          for (int gi = 0; gi < gN; gi++)
          {
            if (strcmp(allIdV[gi], a->idV[k]) == 0)
            {
              anySuccessV[gi] = true;
              break;
            }
          }
        }
      }
    }
  }

  //
  // Pass 4 — bulk DB write of final states.
  //
  if (finalN > 0)
  {
    if (db.entityBulkUpdate == NULL)
    {
      ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
              "Batch Entity Update not supported by this DB plugin");
      return true;
    }

    int* resultsV = (int*) kaAlloc(&corRest.kalloc, sizeof(int) * finalN);
    db.entityBulkUpdate(tenantP, finals, resultsV);

    for (int ix = 0; ix < doneN; ix++)
      bridgeRequestsWritten(doneV[ix]);             // late replies and goals: released after the notifications

    for (int k = 0; k < finalN; k++)
    {
      const char* eid = finalIdV[k];
      switch (resultsV[k])
      {
        case DB_OK:
        {
          // Mark success on the top-level index tracker
          for (int gi = 0; gi < gN; gi++)
            if (strcmp(allIdV[gi], eid) == 0) { anySuccessV[gi] = true; break; }
          break;
        }
        case DB_NOT_FOUND:
          addBatchError(errorsP, eid, 404,
                        LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                        "entity vanished between retrieve and bulk update", NULL);
          break;
        case DB_GEO_TYPE_CONFLICT:
          addBatchError(errorsP, eid, 409,
                        LD_ERROR_CONFLICT, "Attribute Type Conflict",
                        "an Attribute name is already in use with a conflicting Attribute type in this tenant "
                        "(a GeoProperty and another type cannot share one Attribute name here)", NULL);
          break;
        default:
          addBatchError(errorsP, eid, 500,
                        LD_ERROR_INTERNAL_ERROR, "Internal Error",
                        "database error during batch update", NULL);
          break;
      }
    }
  }

  //
  // Pass 5 — response assembly.
  //
  for (int gi = 0; gi < gN; gi++)
  {
    if (anySuccessV[gi])
      corTreeChildAdd(successP, corTreeString(corRest.kallocP, NULL, (char*) allIdV[gi]));
  }

  int successCount = 0;
  for (CorNode* p = successP->value.firstChildP; p != NULL; p = p->next) successCount++;

  int errorCount = 0;
  for (CorNode* p = errorsP->value.firstChildP; p != NULL; p = p->next) errorCount++;

  // § 5.6.10 — batch update returns 204 when there are no errors. The
  // (success=0, errors=0) case (e.g. an empty input array) is success.
  if (errorCount == 0)
  {
    corRest.out.httpStatusCode = 204;
    return true;
  }

  int singleStatus = (successCount == 0) ? ldBatchErrorsSingleStatus(errorsP) : -1;
  if (singleStatus > 0)
  {
    corRest.out.responseTree   = ldBatchErrorAsProblemDetails(errorsP);
    corRest.out.httpStatusCode = singleStatus;
  }
  else
  {
    CorNode* respBodyP = corTreeObject(corRest.kallocP, NULL);
    corTreeChildAdd(respBodyP, successP);
    corTreeChildAdd(respBodyP, errorsP);
    corRest.out.responseTree = respBodyP;
    corRest.out.httpStatusCode = 207;
  }
  corNgsild.rawResponse      = true;
  return true;
}

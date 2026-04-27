//
// FILE            postEntities.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <string.h>                                  // strlen, strcpy, strcat, strcasecmp
#include <strings.h>                                 // strcasecmp
#include <stdlib.h>                                  // free
#include <stdint.h>                                  // uint64_t
#include <time.h>                                    // clock_gettime

#include "swRest/SwRestState.h"                      // swRest
#include "swRest/SwRestVerb.h"                       // SwVerbPost
#include "swRest/swRestOutHeader.h"                  // swRestOutHeaderAdd

#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjClone.h"                           // kjClone
#include "kjson/kjBuilder.h"                         // kjChildAdd, kjChildRemove, kjString, kjArray, kjObject
#include "kjson/KjNode.h"                            // KjNode, KjString
#include "kjson/kjRender.h"                          // kjFastRender
#include "kjson/kjRenderSize.h"                      // kjFastRenderSize

#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "swJsonld/swldInit.h"                       // swldCoreContext, SWLD_CORE_CONTEXT_URL
#include "swJsonld/SwldContext.h"                    // SwldContext
#include "swJsonld/swldCompact.h"                    // swldCompact

#include "swNgsild/swNgsild.h"                       // ldError, ldCheckEntity, LdOp, LD_ERROR_*, swNgsild
#include "swNgsild/ldCheckEntity.h"                  // ldCheckEntity
#include "swNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "swNgsild/LdProblem.h"                      // LD_ERROR_CONFLICT

#include "swNgsild/LdSubCache.h"                     // LdSubCache
#include "swNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityCreate
#include "swNgsild/ldNotifyDefer.h"                  // ldNotifyDefer

#include "troe/TroeDriver.h"                         // troe, TroeEvent, TroeOpEntityCreated
#include "troe/troeDispatch.h"                       // troeDeferEntityEvent

#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode, LdRegInfo
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieve, ldRegOpSupported
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant, ldViaHasAlias
#include "swNgsild/ldDistOp.h"                       // ldDistOpLoopDetected
#include "swNgsild/ldEntityFragment.h"               // ldEntityFragmentForInfo

#include "db/DbDriver.h"                             // db, DB_OK, DB_ALREADY_EXISTS
#include "db/Tenant.h"                               // Tenant
#include "swNgsild/LdGeoRel.h"                       // LdGeoRel, LdGeoWithin

#include "serviceRoutines/postEntities.h"            // Own interface



// -----------------------------------------------------------------------------
//
// hasNonKeywordAttr - true if entityP has any top-level attribute that is
// not a reserved keyword (id / type / @-prefixed). Used to detect "has
// distops consumed every attribute?"
//
static bool hasNonKeywordAttr(KjNode* entityP)
{
  if (entityP == NULL || entityP->type != KjObject)
    return false;

  for (KjNode* curP = entityP->value.firstChildP; curP != NULL; curP = curP->next)
  {
    if (curP->name == NULL)                       continue;
    if (curP->name[0] == '@')                     continue;
    if (strcmp(curP->name, "id")   == 0)          continue;
    if (strcmp(curP->name, "type") == 0)          continue;
    return true;
  }
  return false;
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
// wrapApiGeoPropAsStorage - synthesize a minimal storage-format entity that
// holds just one GeoProperty, for passing to db.geoMatchFunc.
//
// Dispatch runs on the API-form entity (pre-ldApiEntityToDbModel), so the
// entity's `location` is `{type: "GeoProperty", value: {...}}` at top level.
// db.geoMatchFunc's entityGeoPropGet expects storage-format — wrap as
// `{ <propIri>: { @none: {type: "GeoProperty", value: {...}} } }`.
//
// Returns NULL if the entity has no attribute at apiPropName (or it's
// malformed) — caller treats that as "no geo property on entity".
//
static KjNode* wrapApiGeoPropAsStorage(KjNode* entityP, const char* apiPropName,
                                       const char* storagePropIri)
{
  KjNode* attrP = kjLookup(entityP, apiPropName);
  if (attrP == NULL || attrP->type != KjObject)
    return NULL;

  KjNode* synthetic = kjObject(swRest.kjsonP, NULL);
  KjNode* prop      = kjObject(swRest.kjsonP, (char*) storagePropIri);
  KjNode* wrapInst  = kjObject(swRest.kjsonP, "@none");

  // Borrow children of the API attr (type / value) into @none wrapper
  for (KjNode* c = attrP->value.firstChildP; c != NULL; c = c->next)
  {
    KjNode* linked = kjClone(swRest.kjsonP, c);
    kjChildAdd(wrapInst, linked);
  }

  kjChildAdd(prop, wrapInst);
  kjChildAdd(synthetic, prop);
  return synthetic;
}



// -----------------------------------------------------------------------------
//
// csrGeoCoverEntity - does this CSR's geo coverage admit the entity?
//
// For each of the three geo fields the CSR may declare
// (location / observationSpace / operationSpace), check that the
// entity's matching GeoProperty lies within the CSR's declared
// boundary. Uses the DB plugin's registered GEOS callback so
// swNgsild/swBroker main need no direct GEOS dependency.
//
// Behaviour:
//   - CSR has no geo fields → match (not restricted).
//   - db.geoMatchFunc unavailable → match (cannot enforce; conservative).
//   - CSR has a geo field but entity lacks the corresponding GeoProperty
//     → NO match (geo-scoped CSR cannot apply to a location-less entity).
//
static bool csrGeoCoverEntity(LdRegCacheItem* csr, KjNode* entityP)
{
  if (csr->locationP == NULL && csr->observationSpaceP == NULL && csr->operationSpaceP == NULL)
    return true;

  if (db.geoMatchFunc == NULL)
    return true;

  // Pair each CSR geo field with the entity's API-form attr name and its
  // expanded IRI (as used by db.geoMatchFunc internally on stored entities).
  struct {
    KjNode*      csrGeom;
    const char*  apiName;
    const char*  storageIri;
  } pairs[] = {
    { csr->locationP,         "location",         "https://uri.etsi.org/ngsi-ld/location"         },
    { csr->observationSpaceP, "observationSpace", "https://uri.etsi.org/ngsi-ld/observationSpace" },
    { csr->operationSpaceP,   "operationSpace",   "https://uri.etsi.org/ngsi-ld/operationSpace"   },
  };

  LdGeoRel georel = { LdGeoWithin, -1, -1 };

  for (int i = 0; i < 3; i++)
  {
    if (pairs[i].csrGeom == NULL)
      continue;

    KjNode* typeP   = kjLookup(pairs[i].csrGeom, "type");
    KjNode* coordsP = kjLookup(pairs[i].csrGeom, "coordinates");
    if (typeP == NULL || typeP->type != KjString || coordsP == NULL)
      continue;

    KjNode* synthetic = wrapApiGeoPropAsStorage(entityP, pairs[i].apiName, pairs[i].storageIri);
    if (synthetic == NULL)
      return false;   // entity has no matching geo property → fail this CSR

    int   cbSize = kjFastRenderSize(coordsP) + 1;
    char* cbuf   = (char*) kaAlloc(&swRest.kalloc, cbSize);
    kjFastRender(coordsP, cbuf);

    if (!db.geoMatchFunc(synthetic, &georel, typeP->value.s, cbuf, pairs[i].storageIri))
      return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// renderFragmentWithContext - serialize fragment body with @context for remote
//
// The fragment's attr names are expanded IRIs. We attach a @context that
// points to the core context URL so the remote broker parses the body as
// valid JSON-LD. Expanded IRIs either already match the core context's
// vocab terms (and compact back automatically) or remain as IRIs — both
// are acceptable JSON-LD.
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
// fragmentShortAttrList - comma-separated compacted attr names from a fragment
//
// Helper for building the ProblemDetails.detail string — lists which
// attributes of the entity were affected by this failure.
//
static const char* fragmentShortAttrList(KjNode* fragP)
{
  static __thread char buf[256];
  int pos = 0;

  if (fragP == NULL)
  {
    buf[0] = 0;
    return buf;
  }

  for (KjNode* c = fragP->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->name == NULL)                       continue;
    if (c->name[0] == '@')                     continue;
    if (strcmp(c->name, "id")   == 0)          continue;
    if (strcmp(c->name, "type") == 0)          continue;

    const char* compact = swldCompact(swldCoreContext(), c->name);
    const char* shortN  = (compact != NULL) ? compact : c->name;

    int nlen = strlen(shortN);
    if (pos + nlen + 2 >= (int) sizeof(buf))
      break;

    if (pos > 0)
    {
      buf[pos++] = ',';
      buf[pos++] = ' ';
    }
    memcpy(buf + pos, shortN, nlen);
    pos += nlen;
  }

  buf[pos] = 0;
  return buf;
}



// -----------------------------------------------------------------------------
//
// appendBatchEntityError - append one BatchEntityError (§ 5.2.17) to errors[]
//
// Per-CSR granularity (NOT per-attribute): each failed forward contributes
// one entry, with the ProblemDetails embedded as `error`. The affected
// attribute list is baked into the ProblemDetails.detail string since
// v1.9.1 ProblemDetails has no native attribute-list field.
//
static void appendBatchEntityError(KjNode*      errorsArrayP,
                                   const char*  entityId,
                                   const char*  errorType,
                                   const char*  errorTitle,
                                   const char*  errorDetail,
                                   const char*  regId)
{
  if (errorsArrayP == NULL)
    return;

  KjNode* entry = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(entry, kjString(swRest.kjsonP, "entityId", entityId));

  KjNode* pd = kjObject(swRest.kjsonP, "error");
  kjChildAdd(pd, kjString(swRest.kjsonP, "type",   errorType));
  kjChildAdd(pd, kjString(swRest.kjsonP, "title",  errorTitle));
  kjChildAdd(pd, kjString(swRest.kjsonP, "detail", errorDetail));
  kjChildAdd(entry, pd);

  if (regId != NULL)
    kjChildAdd(entry, kjString(swRest.kjsonP, "registrationId", regId));

  kjChildAdd(errorsArrayP, entry);
}



// -----------------------------------------------------------------------------
//
// forwardFailureReason - build a human-readable "reason" for notCreated
//
static const char* forwardFailureReason(int upCode, const char* upErr)
{
  static __thread char buf[256];

  if (upErr != NULL && upErr[0] != 0)
    snprintf(buf, sizeof(buf), "forward failed: %s", upErr);
  else
    snprintf(buf, sizeof(buf), "forward returned status %d", upCode);

  return buf;
}



// -----------------------------------------------------------------------------
//
// forwardCreateEntity - compose URL + body and delegate to ldDistOpSend
//
// Endpoint is host+port only per § 5.2.9 C.3; broker appends
// /ngsi-ld/v1/entities. Body = fragment rendered as compact JSON with
// @context. All header composition and counter updates happen inside
// ldDistOpSend.
//
static int forwardCreateEntity(LdRegCacheItem* csr,
                               KjNode*         fragP,
                               const char*     ownAlias,
                               const char**    errorDetailPP)
{
  const char* path    = "/ngsi-ld/v1/entities";
  int         baseLen = strlen(csr->endpoint);
  int         pathLen = strlen(path);
  char*       url     = (char*) kaAlloc(&swRest.kalloc, baseLen + pathLen + 1);
  strcpy(url, csr->endpoint);
  strcpy(url + baseLen, path);

  char* body = renderFragmentWithContext(fragP);

  return ldDistOpSend(csr, SwVerbPost, url, body, strlen(body), ownAlias, errorDetailPP);
}



// -----------------------------------------------------------------------------
//
// postEntities -
//
bool postEntities(void)
{
  KjNode* entityP = swRest.in.requestTree;

  //
  // Must have a JSON payload
  //
  //
  // Validate the entity
  //
  if (ldCheckEntity(entityP, LdOpCreateEntity, NULL, &swRest.kalloc) == false)
    return true;

  //
  // Extract entity id
  //
  KjNode* idP = kjLookup(entityP, "id");

  if (idP == NULL || idP->type != KjString)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "entity id is missing or not a string");
    return true;
  }

  const char* entityId = idP->value.s;

  //
  // Snapshot "had non-keyword attributes before dispatch". Used post-
  // dispatch to decide if exclusive/redirect consumed them all — in which
  // case we skip the local create (§ 5.6.1.4: "any remaining input data").
  // An entity that arrived as a pure {id, type} shell stays on the local
  // path (it's a valid create even with no attributes).
  //
  bool inputHadAttrs = hasNonKeywordAttr(entityP);

  //
  // DistOps dispatch (§ 5.6.1 + § 4.3.6.3). Processed ONLY when not
  // ?local=true. Passes: exclusive → redirect → inclusive → local.
  // Auxiliary is retrieve-only (§ 4.3.6.2) — never enters write dispatch.
  //
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  //
  // Failure tracking for partial-success 207 (§ 5.6.1.4 + § 6.4.3.1).
  // Response body is a BatchOperationResult (§ 5.2.17):
  //   { "success": [entityId], "errors": [BatchEntityError] }
  // One BatchEntityError per failed CSR forward; anySucceeded is set if
  // any forward returned 2xx OR the local create succeeded — distinguishes
  // partial-success 207 from complete-failure 409.
  //
  KjNode* errorsArrayP = kjArray(swRest.kjsonP, "errors");
  bool    anySucceeded = false;

  if (swNgsild.local == false && tenantP->regCacheP != NULL)
  {
    // Type vector built from the entity's "type" — string or array per § 4.5.1.
    KjNode* typeP        = kjLookup(entityP, "type");
    char*   typeBuf1[2]  = { NULL, NULL };  // KjString case
    char**  typeArr      = typeBuf1;
    if (typeP != NULL)
    {
      if (typeP->type == KjString)
      {
        typeBuf1[0] = typeP->value.s;
      }
      else if (typeP->type == KjArray)
      {
        int n = 0;
        for (KjNode* t = typeP->value.firstChildP; t != NULL; t = t->next)
          if (t->type == KjString) n++;
        if (n > 0)
        {
          typeArr = (char**) kaAlloc(&swRest.kalloc, (n + 1) * sizeof(char*));
          int ix = 0;
          for (KjNode* t = typeP->value.firstChildP; t != NULL; t = t->next)
            if (t->type == KjString)
              typeArr[ix++] = t->value.s;
          typeArr[ix] = NULL;
        }
      }
    }

    //
    // Scope vector from the entity's top-level "scope" field (§ 4.18).
    // Value may be string or string[]; normalise to NULL-term char* array
    // on the stack for the match call. NULL means "entity has no scope".
    //
    KjNode* scopeP        = kjLookup(entityP, "scope");
    char**  entityScopeV  = NULL;
    char*   scopeBuf1[2]  = { NULL, NULL };   // for KjString case
    if (scopeP != NULL)
    {
      if (scopeP->type == KjString)
      {
        scopeBuf1[0] = scopeP->value.s;
        entityScopeV = scopeBuf1;
      }
      else if (scopeP->type == KjArray)
      {
        int n = 0;
        for (KjNode* s = scopeP->value.firstChildP; s != NULL; s = s->next)
          if (s->type == KjString) n++;
        if (n > 0)
        {
          entityScopeV = (char**) kaAlloc(&swRest.kalloc, (n + 1) * sizeof(char*));
          int ix = 0;
          for (KjNode* s = scopeP->value.firstChildP; s != NULL; s = s->next)
            if (s->type == KjString)
              entityScopeV[ix++] = s->value.s;
          entityScopeV[ix] = NULL;
        }
      }
    }

    LdRegCacheItem** exclV  = NULL;
    LdRegCacheItem** redirV = NULL;
    LdRegCacheItem** inclV  = NULL;
    int exclN  = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  entityId, typeArr, entityScopeV,
                                                  LdRegModeExclusive, &exclV);
    int redirN = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  entityId, typeArr, entityScopeV,
                                                  LdRegModeRedirect, &redirV);
    int inclN  = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  entityId, typeArr, entityScopeV,
                                                  LdRegModeInclusive, &inclV);

    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);
    bool        loopSeen = ldDistOpLoopDetected(ownAlias);

    // Loop detected → skip forwards but still run local create. Freeing
    // the match arrays; dispatch block below is gated on !loopSeen.
    if (loopSeen)
    {
      if (exclV  != NULL) { free(exclV);  exclV  = NULL; exclN  = 0; }
      if (redirV != NULL) { free(redirV); redirV = NULL; redirN = 0; }
      if (inclV  != NULL) { free(inclV);  inclV  = NULL; inclN  = 0; }
    }

    if (exclN > 0 || redirN > 0 || inclN > 0)
    {

      //
      // Exclusive pass — per-CSR, per-RegistrationInfo.
      //
      // Each claimed fragment is chopped from entityP (§ 4.3.6.3:
      // broker must not hold exclusive-claimed Attributes locally).
      //
      // - CSR supports createEntity → forward; on non-2xx, record each
      //   claimed attr in notCreated so the client sees which source
      //   refused and why.
      // - CSR does NOT support createEntity → spec § 5.6.1.4 says this
      //   is an "error of type Conflict ... or a partial success":
      //   the chop stands (invariant wins), and every chopped attr is
      //   recorded in notCreated with reason "createEntity not in
      //   registration's operations". Combined with any other success
      //   this becomes a 207; with no other success, a 409 Conflict.
      //
      for (int i = 0; i < exclN; i++)
      {
        LdRegCacheItem* csr = exclV[i];
        if (!csrGeoCoverEntity(csr, entityP)) continue;
        if (csr->endpoint == NULL)
        {
          ldError(502, LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                  "exclusive registration '%s' has no endpoint", csr->regId);
          if (exclV  != NULL) free(exclV);
          if (redirV != NULL) free(redirV);
          if (inclV  != NULL) free(inclV);
          return true;
        }

        // Proactive loop-detect (§ 5.12): CSR alias known + in chain → skip
        if (ldDistOpCsrWouldLoop(csr, ownAlias))
          continue;

        bool opSupported = ldRegOpSupported(csr, swRest.serviceP->ldOp);

        for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
        {
          if (!entityInfoCoversId(riP, entityId))
            continue;

          KjNode* fragP = ldEntityFragmentForInfo(entityP, riP, swRest.kjsonP, true);
          if (fragP == NULL)
            continue;

          if (!opSupported)
          {
            char detail[384];
            snprintf(detail, sizeof(detail),
                     "exclusive registration does not support createEntity; affected attributes: %s",
                     fragmentShortAttrList(fragP));
            appendBatchEntityError(errorsArrayP, entityId,
                                   LD_ERROR_CONFLICT, "Conflict", detail, csr->regId);
            continue;
          }

          const char* upErr  = NULL;
          int         upCode = forwardCreateEntity(csr, fragP, ownAlias, &upErr);

          if (upCode < 200 || upCode >= 300)
          {
            char detail[512];
            snprintf(detail, sizeof(detail), "%s; affected attributes: %s",
                     forwardFailureReason(upCode, upErr), fragmentShortAttrList(fragP));
            appendBatchEntityError(errorsArrayP, entityId,
                                   LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                                   detail, csr->regId);
          }
          else
            anySucceeded = true;
        }
      }

      //
      // Redirect pass — each CSR independent; attrs chopped per fragment.
      // Failures become BatchEntityError entries just like exclusive.
      //
      for (int i = 0; i < redirN; i++)
      {
        LdRegCacheItem* csr = redirV[i];
        if (!csrGeoCoverEntity(csr, entityP)) continue;
        if (csr->endpoint == NULL)    continue;

        // Proactive loop-detect (§ 5.12): CSR alias known + in chain → skip
        if (ldDistOpCsrWouldLoop(csr, ownAlias))
          continue;

        bool opSupported = ldRegOpSupported(csr, swRest.serviceP->ldOp);

        for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
        {
          if (!entityInfoCoversId(riP, entityId))
            continue;

          KjNode* fragP = ldEntityFragmentForInfo(entityP, riP, swRest.kjsonP, true);
          if (fragP == NULL)
            continue;

          if (!opSupported)
          {
            char detail[384];
            snprintf(detail, sizeof(detail),
                     "redirect registration does not support createEntity; affected attributes: %s",
                     fragmentShortAttrList(fragP));
            appendBatchEntityError(errorsArrayP, entityId,
                                   LD_ERROR_CONFLICT, "Conflict", detail, csr->regId);
            continue;
          }

          const char* upErr  = NULL;
          int         upCode = forwardCreateEntity(csr, fragP, ownAlias, &upErr);

          if (upCode < 200 || upCode >= 300)
          {
            char detail[512];
            snprintf(detail, sizeof(detail), "%s; affected attributes: %s",
                     forwardFailureReason(upCode, upErr), fragmentShortAttrList(fragP));
            appendBatchEntityError(errorsArrayP, entityId,
                                   LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                                   detail, csr->regId);
          }
          else
            anySucceeded = true;
        }
      }

      //
      // Inclusive pass — forward each, KEEP attrs on entityP for local store.
      // Inclusive failures still become BatchEntityError entries — the remote
      // copy didn't land even though the local copy will — so the client
      // can see per-source divergence.
      //
      for (int i = 0; i < inclN; i++)
      {
        LdRegCacheItem* csr = inclV[i];
        if (!csrGeoCoverEntity(csr, entityP)) continue;
        if (csr->endpoint == NULL)                   continue;
        if (ldDistOpCsrWouldLoop(csr, ownAlias))     continue;  // § 5.12 proactive
        if (!ldRegOpSupported(csr, swRest.serviceP->ldOp)) continue;

        for (LdRegInfo* riP = csr->infoV; riP != NULL; riP = riP->next)
        {
          if (!entityInfoCoversId(riP, entityId))
            continue;

          KjNode* fragP = ldEntityFragmentForInfo(entityP, riP, swRest.kjsonP, false);
          if (fragP == NULL)
            continue;

          const char* upErr  = NULL;
          int         upCode = forwardCreateEntity(csr, fragP, ownAlias, &upErr);

          if (upCode < 200 || upCode >= 300)
          {
            char detail[512];
            snprintf(detail, sizeof(detail), "%s; affected attributes: %s",
                     forwardFailureReason(upCode, upErr), fragmentShortAttrList(fragP));
            appendBatchEntityError(errorsArrayP, entityId,
                                   LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                                   detail, csr->regId);
          }
          else
            anySucceeded = true;
        }
      }

      if (exclV  != NULL) free(exclV);
      if (redirV != NULL) free(redirV);
      if (inclV  != NULL) free(inclV);
    }
  }

  //
  // Local store — § 5.6.1.4 "Any remaining input data shall be used to
  // create the Entity locally". Skip the store (but not the AlreadyExists
  // check) when exclusive/redirect dispatch chopped every input attr.
  //
  bool distopsConsumedAll = (inputHadAttrs && !hasNonKeywordAttr(entityP));
  bool localAlreadyExists = false;
  bool localCreatedOk     = false;

  if (distopsConsumedAll)
  {
    KjNode* existing = NULL;
    int     rr       = db.entityRetrieve(tenantP, idP->value.s, &existing);
    if (rr == DB_OK)
      localAlreadyExists = true;
  }
  else
  {
    ldApiEntityToDbModel(entityP, &swRest.kalloc);

    int r = db.entityCreate(tenantP, idP->value.s, entityP);

    if (r == DB_ALREADY_EXISTS)
    {
      localAlreadyExists = true;
    }
    else if (r == DB_OK)
    {
      localCreatedOk = true;
      anySucceeded   = true;

      // mongocKjTreeToBson renames "id" to "_id" in-place — restore it.
      if (idP->name[0] == '_')
        idP->name = "id";

      if (tenantP->subCacheP != NULL)
        ldNotifyDefer((LdSubCache*) tenantP->subCacheP, entityP, LdNotifyEntityCreate, NULL);

      // TRoE: defer one entity-level "created" event. The plugin walks
      // entitySnapshot at dispatch time to materialize per-attribute rows.
      KjNode* typeNode = kjLookup(entityP, "type");
      const char* etype = (typeNode != NULL && typeNode->type == KjString) ? typeNode->value.s : NULL;
      TroeEvent* tevP = (TroeEvent*) kaAlloc(&swRest.kalloc, sizeof(TroeEvent));
      memset(tevP, 0, sizeof(*tevP));
      tevP->op             = TroeOpEntityCreated;
      tevP->tenantP        = tenantP;
      tevP->entityId       = idP->value.s;
      tevP->entityType     = etype;
      tevP->modifiedAtNs   = swRest.requestStartTime;
      tevP->entitySnapshot = entityP;
      troeDeferEntityEvent(tevP);
    }
    else
    {
      //
      // Local DB error — if any forward succeeded report a partial-success
      // 207 with a BatchEntityError for the local leg. If no forward
      // succeeded either, fall through to the normal 500.
      //
      if (anySucceeded)
      {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "local database error while creating entity '%s'", idP->value.s);
        appendBatchEntityError(errorsArrayP, idP->value.s,
                               LD_ERROR_INTERNAL_ERROR, "Internal Error",
                               detail, NULL);
        // fall through to the response-decision matrix below
      }
      else
      {
        ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
                "database error creating entity '%s'", idP->value.s);
        return true;
      }
    }
  }

  //
  // Response decision matrix (§ 6.4.3.1):
  //   - Local AlreadyExists AND nothing else succeeded → 409 AlreadyExists.
  //   - errors[] empty AND (localCreatedOk OR distopsConsumedAll+noLocal)
  //     → 201 Created, no body.
  //   - errors[] non-empty AND anySucceeded → 207 Multi-Status with
  //     BatchOperationResult body.
  //   - errors[] non-empty AND nothing succeeded → 409 Conflict with
  //     the same BatchOperationResult body so the client sees per-CSR
  //     failures.
  //
  int errorsCount = 0;
  for (KjNode* p = errorsArrayP->value.firstChildP; p != NULL; p = p->next) errorsCount++;

  if (localAlreadyExists && !anySucceeded)
  {
    ldError(409, LD_ERROR_ALREADY_EXISTS, "Already Exists",
            "entity '%s' already exists", idP->value.s);
    return true;
  }

  if (errorsCount == 0)
  {
    // 201 Created -- set Location and Link headers, no body
    swRest.out.httpStatusCode = 201;

    // Per § 5.6.1 the Location must be a URI identifying the created resource.
    // That means the full API path /ngsi-ld/v1/entities/{id}, not just the id.
    const char* prefix  = "/ngsi-ld/v1/entities/";
    int         locLen  = strlen(prefix) + strlen(idP->value.s) + 1;
    char*       locBuf  = kaAlloc(&swRest.kalloc, locLen);
    strcpy(locBuf, prefix);
    strcat(locBuf, idP->value.s);
    swRestOutHeaderAdd("Location", locBuf);

    SwldContext* ctxP = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();
    const char*  ctxUrl = ctxP->url;

    if (ctxUrl != NULL)
    {
      const char* suffix  = ">; rel=\"http://www.w3.org/ns/json-ld#context\"; type=\"application/ld+json\"";
      int         linkLen = 1 + strlen(ctxUrl) + strlen(suffix) + 1;
      char*       linkBuf = kaAlloc(&swRest.kalloc, linkLen);

      strcpy(linkBuf, "<");
      strcat(linkBuf, ctxUrl);
      strcat(linkBuf, suffix);
      swRestOutHeaderAdd("Link", linkBuf);
    }

    return true;
  }

  //
  // Build BatchOperationResult response body (§ 5.2.17).
  // If the entity was AT LEAST partially created (some forward succeeded
  // OR local create succeeded), list its URI in success[]. Otherwise
  // success[] is empty.
  //
  // If local AlreadyExists AND some forwards succeeded, add a
  // BatchEntityError for the local leg so the client knows why local
  // didn't land.
  //
  if (localAlreadyExists)
  {
    appendBatchEntityError(errorsArrayP, idP->value.s,
                           LD_ERROR_ALREADY_EXISTS, "Already Exists",
                           "entity already exists locally", NULL);
  }

  KjNode* successArrayP = kjArray(swRest.kjsonP, "success");
  if (anySucceeded)
    kjChildAdd(successArrayP, kjString(swRest.kjsonP, NULL, idP->value.s));

  KjNode* respBodyP = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(respBodyP, successArrayP);
  kjChildAdd(respBodyP, errorsArrayP);

  swRest.out.responseTree   = respBodyP;
  swRest.out.httpStatusCode = anySucceeded ? 207 : 409;

  return true;
}

//
// FILE            purgeEntities.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// DELETE /ngsi-ld/v1/entities — Purge Entities (§ 5.6.21 / § 6.4.3.3).
//
// Query-based deletion:
//   - filters (type / id / idPattern / attrs / q / geoquery / scopeQ)
//     select the target set;
//   - `drop=a,b` deletes only the listed attributes from each matching
//     entity (partial purge);
//   - `keep=a,b` deletes every attribute EXCEPT the listed ones;
//   - neither → whole entity is deleted.
//
// At least one non-trivial filter must be provided unless ?local=true
// (§ 5.6.21.4). Distops: forward the same URL to every matching CSR
// that supports the purgeEntity operation. Partial success → 207 with
// BatchOperationResult (§ 5.2.17).
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp, strlen, strcpy, memcpy
#include <stdlib.h>                                  // free
#include <stdio.h>                                   // snprintf

#include "swRest/SwRestState.h"                      // swRest
#include "swRest/SwRestVerb.h"                       // SwVerbDelete

#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjArray, kjString, kjChildAdd
#include "kjson/kjLookup.h"                          // kjLookup
#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "swJsonld/swldExpand.h"                     // swldExpand

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdProblem.h"                      // LD_ERROR_CONFLICT
#include "swNgsild/LdSubCache.h"                     // LdSubCache
#include "swNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityDelete
#include "swNgsild/ldNotifyDefer.h"                  // ldNotifyDefer
#include "swNgsild/ldIsEntityKeyword.h"              // ldIsEntityKeyword
#include "swNgsild/LdVocab.h"                        // LD_VOCAB_NGSILD_NULL

#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode
#include "swNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "swNgsild/ldDistOp.h"                       // ldDistOpLoopDetected, ldDistOpSend, ldDistOpBatchErrorAdd

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/DbQueryFilter.h"                        // DbQueryFilter
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/purgeEntities.h"           // Own interface



// -----------------------------------------------------------------------------
//
// rebuildQueryString - regenerate a query string from swRest.in.uriParamV
//
// swRest's URL-params parser mutates the original ?query= buffer
// in-place during parsing, so swRest.in.urlParams is corrupted by the
// time a service routine sees it. We rebuild from the parsed KV array.
//
static char* rebuildQueryString(void)
{
  int total = 1;   // NUL
  for (int i = 0; i < swRest.in.uriParamCount; i++)
  {
    const char* k = swRest.in.uriParamV[i].key;
    const char* v = swRest.in.uriParamV[i].value;
    if (k == NULL) continue;
    total += strlen(k) + 1;                   // key + '='
    if (v != NULL) total += strlen(v);        // value — already percent-decoded
    total += 1;                               // '&'
  }

  char* buf = (char*) kaAlloc(&swRest.kalloc, total);
  buf[0] = '\0';

  bool first = true;
  for (int i = 0; i < swRest.in.uriParamCount; i++)
  {
    const char* k = swRest.in.uriParamV[i].key;
    const char* v = swRest.in.uriParamV[i].value;
    if (k == NULL) continue;

    if (!first) strcat(buf, "&");
    strcat(buf, k);
    strcat(buf, "=");
    if (v != NULL) strcat(buf, v);
    first = false;
  }

  return buf;
}



// -----------------------------------------------------------------------------
//
// forwardUrl - "<endpoint>/ngsi-ld/v1/entities?<rebuilt-query>"
//
// Receiver applies § 5.6.21 semantics against its own local scope + CSRs.
//
static char* forwardUrl(const char* endpoint)
{
  const char* path    = "/ngsi-ld/v1/entities";
  int         baseLen = strlen(endpoint);
  int         pathLen = strlen(path);
  char*       query   = rebuildQueryString();
  int         qLen    = strlen(query);
  int         total   = baseLen + pathLen + (qLen > 0 ? 1 + qLen : 0) + 1;

  char* url = (char*) kaAlloc(&swRest.kalloc, total);
  strcpy(url, endpoint);
  strcpy(url + baseLen, path);
  if (qLen > 0)
  {
    url[baseLen + pathLen] = '?';
    strcpy(url + baseLen + pathLen + 1, query);
  }
  return url;
}



// -----------------------------------------------------------------------------
//
// inStringV - true if `s` is in a NULL-terminated string array
//
static bool inStringV(char** strV, const char* s)
{
  if (strV == NULL) return false;
  for (int i = 0; strV[i] != NULL; i++)
    if (strcmp(strV[i], s) == 0)
      return true;
  return false;
}



// -----------------------------------------------------------------------------
//
// partialPurge - delete a subset of an entity's attrs by posting a
// null-marker fragment to db.entityAttrsSet.
//
// For dropV: fragment = { attr1: "urn:ngsi-ld:null", attr2: ... }.
// For keepV: we need the stored entity to know which attrs to null-out.
//
// Returns DB_OK / DB_NOT_FOUND / DB_ERR.
//
static int partialPurge(Tenant* tenantP, const char* entityId, char** dropV, char** keepV)
{
  if (db.entityAttrsSet == NULL)
    return DB_ERR;

  KjNode* fragment = kjObject(swRest.kjsonP, NULL);

  if (dropV != NULL)
  {
    for (int i = 0; dropV[i] != NULL; i++)
    {
      const char* iri = swldExpand(swNgsild.contextP, dropV[i], &swRest.kalloc, NULL, NULL);
      if (iri == NULL) iri = dropV[i];
      kjChildAdd(fragment, kjString(swRest.kjsonP, iri, LD_VOCAB_NGSILD_NULL));
    }
  }
  else if (keepV != NULL)
  {
    KjNode* entityP = NULL;
    int r = db.entityRetrieve(tenantP, entityId, &entityP);
    if (r != DB_OK || entityP == NULL)
      return r;

    // Build expanded keep-list once.
    int    keepN = 0;
    while (keepV[keepN] != NULL) keepN++;

    const char** keepIri = (const char**) kaAlloc(&swRest.kalloc, sizeof(char*) * (keepN + 1));
    for (int i = 0; i < keepN; i++)
    {
      const char* iri = swldExpand(swNgsild.contextP, keepV[i], &swRest.kalloc, NULL, NULL);
      keepIri[i] = (iri != NULL) ? iri : keepV[i];
    }
    keepIri[keepN] = NULL;

    for (KjNode* attrP = entityP->value.firstChildP; attrP != NULL; attrP = attrP->next)
    {
      if (ldIsEntityKeyword(attrP->name)) continue;
      if (inStringV((char**) keepIri, attrP->name)) continue;

      kjChildAdd(fragment, kjString(swRest.kjsonP, attrP->name, LD_VOCAB_NGSILD_NULL));
    }
  }

  // Empty fragment → nothing to do
  if (fragment->value.firstChildP == NULL)
    return DB_OK;

  LdMergeReport report = { NULL };
  return db.entityAttrsSet(tenantP, entityId, fragment, true,
                           swRest.requestStartTime, &report);
}



// -----------------------------------------------------------------------------
//
// hasRestrictiveFilter - § 5.6.21.4: at least one filter or ?local=true
//
static bool hasRestrictiveFilter(void)
{
  if (swNgsild.idV        != NULL) return true;
  if (swNgsild.idPattern  != NULL) return true;
  if (swNgsild.typeV      != NULL) return true;
  if (swNgsild.typeExpr   != NULL) return true;
  if (swNgsild.qExpr      != NULL) return true;
  if (swNgsild.scopeExpr  != NULL) return true;
  if (swNgsild.geoRel     != NULL) return true;
  if (swNgsild.geometry   != NULL) return true;
  if (swNgsild.coordinates != NULL) return true;
  return false;
}



// -----------------------------------------------------------------------------
//
// purgeEntities -
//
bool purgeEntities(void)
{
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  //
  // Spec guard: at least one restrictive param, OR ?local=true.
  //
  if (!swNgsild.local && !hasRestrictiveFilter())
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Purge Entities requires at least one of id, type, idPattern, q, geoquery, scopeQ — or ?local=true");
    return true;
  }

  //
  // DistOps dispatch. Skipped when ?local=true or no regCache. Unlike
  // the write ops, purge forwards the *same URL* to each matching CSR:
  // the receiver applies § 5.6.21 locally within its own registry scope.
  //
  KjNode* errorsArrayP = kjArray(swRest.kjsonP, "errors");
  KjNode* successArrayP = kjArray(swRest.kjsonP, "success");

  bool dispatch = (swNgsild.local == false
                   && tenantP != NULL
                   && tenantP->regCacheP != NULL);

  const char* ownAlias = (tenantP != NULL)
                         ? ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc)
                         : NULL;

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;

  if (dispatch)
  {
    char** typeArr = swNgsild.typeV;

    LdRegCacheItem** exclV  = NULL;
    LdRegCacheItem** redirV = NULL;
    LdRegCacheItem** inclV  = NULL;
    int exclN  = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  NULL, typeArr, NULL,
                                                  LdRegModeExclusive, &exclV);
    int redirN = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  NULL, typeArr, NULL,
                                                  LdRegModeRedirect, &redirV);
    int inclN  = ldRegCacheMatchForRetrieveScoped((LdRegCache*) tenantP->regCacheP,
                                                  NULL, typeArr, NULL,
                                                  LdRegModeInclusive, &inclV);

    LdRegCacheItem** groups[]     = { exclV,       redirV,     inclV      };
    int              counts[]     = { exclN,       redirN,     inclN      };
    const char*      modeTag[]    = { "exclusive", "redirect", "inclusive" };
    bool             opConflict[] = { true,        true,       false      };

    for (int g = 0; g < 3; g++)
    {
      for (int i = 0; i < counts[g]; i++)
      {
        LdRegCacheItem* csr = groups[g][i];
        if (csr->endpoint == NULL)                  continue;
        if (ldDistOpCsrWouldLoop(csr, ownAlias))    continue;

        if (!ldRegOpSupported(csr, "purgeEntity"))
        {
          if (!opConflict[g])
            continue;
          char detail[256];
          snprintf(detail, sizeof(detail),
                   "%s registration does not support purgeEntity", modeTag[g]);
          ldDistOpBatchErrorAdd(errorsArrayP, csr->regId,
                                LD_ERROR_CONFLICT, "Conflict", detail, csr->regId);
          continue;
        }

        const char* upErr  = NULL;
        int         upCode = ldDistOpSend(csr, SwVerbDelete,
                                          forwardUrl(csr->endpoint),
                                          NULL, 0, ownAlias, &upErr);

        if (upCode >= 200 && upCode < 300)
          kjChildAdd(successArrayP, kjString(swRest.kjsonP, NULL, csr->regId));
        else if (upCode != 404)
          ldDistOpBatchErrorAdd(errorsArrayP, csr->regId,
                                LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                                ldDistOpForwardFailureReason(upCode, upErr), csr->regId);
      }
    }

    if (exclV  != NULL) free(exclV);
    if (redirV != NULL) free(redirV);
    if (inclV  != NULL) free(inclV);
  }

  //
  // Local purge. Query matching entities, then delete (or partial-delete
  // via drop/keep null-fragment).
  //
  if (db.entityQuery != NULL)
  {
    DbQueryFilter filter = {0};
    filter.idV         = swNgsild.idV;
    filter.idPattern   = swNgsild.idPattern;
    filter.typeV       = swNgsild.typeV;
    filter.typeExpr    = swNgsild.typeExpr;
    filter.scopeExpr   = swNgsild.scopeExpr;
    filter.qExpr       = swNgsild.qExpr;
    filter.geoRel      = swNgsild.geoRel;
    filter.geometry    = swNgsild.geometry;
    filter.coordinates = swNgsild.coordinates;
    filter.geoproperty = swNgsild.geoproperty
                         ? swNgsild.geoproperty
                         : swldExpand(swNgsild.contextP, "location", &swRest.kalloc, NULL, NULL);
    filter.limit       = 1000000;  // effectively unlimited — every matching entity
    filter.offset      = 0;
    filter.count       = false;

    KjNode* arrayP = NULL;
    int     qr     = db.entityQuery(tenantP, &filter, &arrayP);

    if (qr == DB_OK && arrayP != NULL)
    {
      bool partial = (swNgsild.dropV != NULL) || (swNgsild.keepV != NULL);

      for (KjNode* eP = arrayP->value.firstChildP; eP != NULL; eP = eP->next)
      {
        KjNode* idP = kjLookup(eP, "id");
        if (idP == NULL || idP->type != KjString) continue;
        const char* entityId = idP->value.s;

        if (partial)
        {
          int pr = partialPurge(tenantP, entityId, swNgsild.dropV, swNgsild.keepV);
          if (pr == DB_OK)
            kjChildAdd(successArrayP, kjString(swRest.kjsonP, NULL, entityId));
          else if (pr != DB_NOT_FOUND)
            ldDistOpBatchErrorAdd(errorsArrayP, entityId,
                                  LD_ERROR_INTERNAL_ERROR, "Internal Error",
                                  "partial-purge db error", NULL);
        }
        else
        {
          // Pre-fetch for subscription notification
          KjNode* preImage = NULL;
          if (tenantP != NULL && tenantP->subCacheP != NULL)
            db.entityRetrieve(tenantP, entityId, &preImage);

          int dr = db.entityDelete(tenantP, entityId);
          if (dr == DB_OK)
          {
            kjChildAdd(successArrayP, kjString(swRest.kjsonP, NULL, entityId));
            if (tenantP != NULL && tenantP->subCacheP != NULL && preImage != NULL)
              ldNotifyDefer((LdSubCache*) tenantP->subCacheP, preImage, LdNotifyEntityDelete, NULL);
          }
          else if (dr != DB_NOT_FOUND)
          {
            ldDistOpBatchErrorAdd(errorsArrayP, entityId,
                                  LD_ERROR_INTERNAL_ERROR, "Internal Error",
                                  "db error on purge", NULL);
          }
        }
      }
    }
  }

  //
  // Response:
  //   - errors[] empty → 204 No Content
  //   - errors[] non-empty → 207 Multi-Status + BatchOperationResult body
  //
  int errorsCount = 0;
  for (KjNode* p = errorsArrayP->value.firstChildP; p != NULL; p = p->next) errorsCount++;

  if (errorsCount == 0)
  {
    swRest.out.httpStatusCode = 204;
    return true;
  }

  KjNode* respBodyP = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(respBodyP, successArrayP);
  kjChildAdd(respBodyP, errorsArrayP);

  swRest.out.responseTree   = respBodyP;
  swRest.out.httpStatusCode = 207;
  return true;
}

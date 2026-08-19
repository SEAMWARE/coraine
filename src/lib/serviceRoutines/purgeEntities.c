//
// FILE            purgeEntities.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
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

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/CorRestVerb.h"                       // CorVerbDelete

#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjArray, kjString, kjChildAdd
#include "kjson/kjLookup.h"                          // kjLookup
#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "corJsonld/corLdExpand.h"                     // corLdExpand

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdProblem.h"                      // LD_ERROR_CONFLICT
#include "corNgsild/LdSubCache.h"                     // LdSubCache
#include "corNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityDelete
#include "corNgsild/ldNotifyDefer.h"                  // ldNotifyDefer
#include "corNgsild/ldIsEntityKeyword.h"              // ldIsEntityKeyword
#include "corNgsild/LdVocab.h"                        // LD_VOCAB_NGSILD_NULL

#include "corNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode
#include "corNgsild/ldRegCache.h"                     // ldRegCacheMatchForRetrieveScoped, ldRegOpSupported
#include "corNgsild/ldEntityMatch.h"                  // ldEntityMatchQ
#include "corNgsild/ldTraceLevels.h"                  // LdTRegMatch
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "corNgsild/ldDistOp.h"                       // ldDistOpLoopDetected, ldDistOpSend, ldDistOpBatchErrorAdd

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/DbQueryFilter.h"                        // DbQueryFilter
#include "db/Tenant.h"                               // Tenant

#include "ktrace/kTrace.h"                           // KT_T
#include "coraineTraceLevels.h"                     // KtDistOpRequest

#include "serviceRoutines/purgeEntities.h"           // Own interface



// -----------------------------------------------------------------------------
//
// rebuildQueryString - regenerate a query string from corRest.in.uriParamV
//
// corRest's URL-params parser mutates the original ?query= buffer
// in-place during parsing, so corRest.in.urlParams is corrupted by the
// time a service routine sees it. We rebuild from the parsed KV array.
//
static char* rebuildQueryString(void)
{
  int total = 1;   // NUL
  for (int i = 0; i < corRest.in.uriParamCount; i++)
  {
    const char* k = corRest.in.uriParamV[i].key;
    const char* v = corRest.in.uriParamV[i].value;
    if (k == NULL) continue;
    total += strlen(k) + 1;                   // key + '='
    if (v != NULL) total += strlen(v);        // value — already percent-decoded
    total += 1;                               // '&'
  }

  char* buf = (char*) kaAlloc(&corRest.kalloc, total);
  buf[0] = '\0';

  bool first = true;
  for (int i = 0; i < corRest.in.uriParamCount; i++)
  {
    const char* k = corRest.in.uriParamV[i].key;
    const char* v = corRest.in.uriParamV[i].value;
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

  char* url = (char*) kaAlloc(&corRest.kalloc, total);
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

  KjNode* fragment = kjObject(corRest.kjsonP, NULL);

  if (dropV != NULL)
  {
    for (int i = 0; dropV[i] != NULL; i++)
    {
      const char* iri = corLdExpand(corNgsild.contextP, dropV[i], &corRest.kalloc, NULL, NULL);
      if (iri == NULL) iri = dropV[i];
      kjChildAdd(fragment, kjString(corRest.kjsonP, iri, LD_VOCAB_NGSILD_NULL));
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

    const char** keepIri = (const char**) kaAlloc(&corRest.kalloc, sizeof(char*) * (keepN + 1));
    for (int i = 0; i < keepN; i++)
    {
      const char* iri = corLdExpand(corNgsild.contextP, keepV[i], &corRest.kalloc, NULL, NULL);
      keepIri[i] = (iri != NULL) ? iri : keepV[i];
    }
    keepIri[keepN] = NULL;

    for (KjNode* attrP = entityP->value.firstChildP; attrP != NULL; attrP = attrP->next)
    {
      if (ldIsEntityKeyword(attrP->name)) continue;
      if (inStringV((char**) keepIri, attrP->name)) continue;

      kjChildAdd(fragment, kjString(corRest.kjsonP, attrP->name, LD_VOCAB_NGSILD_NULL));
    }
  }

  // Empty fragment → nothing to do
  if (fragment->value.firstChildP == NULL)
    return DB_OK;

  LdMergeReport report = { NULL };
  return db.entityAttrsSet(tenantP, entityId, fragment, true,
                           corRest.requestStartTime, &report);
}



// -----------------------------------------------------------------------------
//
// hasRestrictiveFilter - § 5.6.21.4: at least one filter or ?local=true
//
static bool hasRestrictiveFilter(void)
{
  if (corNgsild.idV        != NULL) return true;
  if (corNgsild.idPattern  != NULL) return true;
  if (corNgsild.typeV      != NULL) return true;
  if (corNgsild.typeExpr   != NULL) return true;
  if (corNgsild.qExpr      != NULL) return true;
  if (corNgsild.scopeExpr  != NULL) return true;
  if (corNgsild.geoRel     != NULL) return true;
  if (corNgsild.geometry   != NULL) return true;
  if (corNgsild.coordinates != NULL) return true;
  return false;
}



// -----------------------------------------------------------------------------
//
// purgeEntities -
//
bool purgeEntities(void)
{
  Tenant* tenantP = (Tenant*) corNgsild.tenantP;

  //
  // Spec guard: at least one restrictive param, OR ?local=true.
  //
  if (!corNgsild.local && !hasRestrictiveFilter())
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Query Too Broad",
            "Purge Entities requires at least one of id, type, idPattern, q, geoquery, scopeQ — or ?local=true");
    return true;
  }

  //
  // 'keep' and 'drop' are mutually exclusive: 'drop' reduces each entity to NOT
  // contain the listed members, 'keep' reduces it down to ONLY those members —
  // combining them is a contradictory request.
  //
  if ((corNgsild.dropV != NULL) && (corNgsild.keepV != NULL))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Request",
            "the 'keep' and 'drop' query parameters are mutually exclusive and cannot be combined in a single Purge Entities request");
    return true;
  }

  //
  // DistOps dispatch. Skipped when ?local=true or no regCache. Unlike
  // the write ops, purge forwards the *same URL* to each matching CSR:
  // the receiver applies § 5.6.21 locally within its own registry scope.
  //
  KjNode* errorsArrayP = kjArray(corRest.kjsonP, "errors");
  KjNode* successArrayP = kjArray(corRest.kjsonP, "success");

  bool dispatch = (corNgsild.local == false
                  
                   && tenantP->regCacheP != NULL);

  const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);

  if (dispatch && ldDistOpLoopDetected(ownAlias))
    dispatch = false;

  if (dispatch)
  {
    char** typeArr = corNgsild.typeV;

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

    int total = exclN + redirN + inclN;
    LdDistOpBatchItem*   items   = (LdDistOpBatchItem*)   kaAlloc(&corRest.kalloc, total * sizeof(LdDistOpBatchItem));
    memset(items, 0, total * sizeof(LdDistOpBatchItem));
    LdDistOpBatchResult* results = (LdDistOpBatchResult*) kaAlloc(&corRest.kalloc, total * sizeof(LdDistOpBatchResult));
    int                  itemCount = 0;
    memset(results, 0, total * sizeof(LdDistOpBatchResult));

    for (int g = 0; g < 3; g++)
    {
      for (int i = 0; i < counts[g]; i++)
      {
        LdRegCacheItem* csr = groups[g][i];
        if (csr->endpoint == NULL)                  continue;
        if (ldDistOpCsrWouldLoop(csr, ownAlias))    continue;

        //
        // § 5.2.23 csf — the Context Source Filter picks the registrations the
        // purge reaches. It is matched against the registration itself, never
        // against the Entities, so a registration we cannot inspect is out.
        //
        if (corNgsild.csfExpr != NULL)
        {
          if ((csr->regTree == NULL) || !ldEntityMatchQ(csr->regTree, corNgsild.csfExpr))
          {
            KT_T(LdTRegMatch, "%s: matched, but NOT purged: the registration does not match csf '%s'",
                 (csr->regId != NULL) ? csr->regId : "<no id>", corNgsild.csf);
            continue;
          }
        }

        if (!ldRegOpSupported(csr, corRest.serviceP->ldOp))
        {
          if (!opConflict[g]) continue;
          char detail[256];
          snprintf(detail, sizeof(detail),
                   "%s registration does not support purgeEntity", modeTag[g]);
          ldDistOpBatchErrorAdd(errorsArrayP, csr->regId, 409,
                                LD_ERROR_CONFLICT, "Conflict", detail, csr->regId);
          continue;
        }

        items[itemCount].csr     = csr;
        items[itemCount].url     = forwardUrl(csr->endpoint);
        items[itemCount].body    = NULL;
        items[itemCount].bodyLen = 0;
        KT_T(KtDistOpRequest, "forward: DELETE %s", items[itemCount].url);

        itemCount++;
      }
    }

    if (itemCount > 0)
    {
      ldDistOpSendMulti(items, itemCount, CorVerbDelete, ownAlias, results);

      for (int i = 0; i < itemCount; i++)
      {
        int upCode = results[i].statusCode;
        if (upCode >= 200 && upCode < 300)
          kjChildAdd(successArrayP, kjString(corRest.kjsonP, NULL, items[i].csr->regId));
        else if (upCode != 404)
          ldDistOpBatchErrorAdd(errorsArrayP, items[i].csr->regId, (upCode >= 400) ? upCode : 502,
                                LD_ERROR_INTERNAL_ERROR, "Bad Gateway",
                                ldDistOpForwardFailureReason(upCode, results[i].errorDetail),
                                items[i].csr->regId);
      }
    }

    ldRegCacheMatchRelease(exclV,  exclN);
    ldRegCacheMatchRelease(redirV, redirN);
    ldRegCacheMatchRelease(inclV,  inclN);
  }

  //
  // Local purge. Query matching entities, then delete (or partial-delete
  // via drop/keep null-fragment).
  //
  if (db.entityQuery != NULL)
  {
    DbQueryFilter filter = {0};
    filter.idV         = corNgsild.idV;
    filter.idPattern   = corNgsild.idPattern;
    filter.typeV       = corNgsild.typeV;
    filter.typeExpr    = corNgsild.typeExpr;
    filter.scopeExpr   = corNgsild.scopeExpr;
    filter.qExpr       = corNgsild.qExpr;
    filter.geoRel      = corNgsild.geoRel;
    filter.geometry    = corNgsild.geometry;
    filter.coordinates = corNgsild.coordinates;
    filter.geoproperty = corNgsild.geoproperty
                         ? corNgsild.geoproperty
                         : corLdExpand(corNgsild.contextP, "location", &corRest.kalloc, NULL, NULL);
    filter.limit       = 1000000;  // effectively unlimited — every matching entity
    filter.offset      = 0;
    filter.count       = false;

    KjNode* arrayP = NULL;
    int     qr     = db.entityQuery(tenantP, &filter, &arrayP);

    if (qr == DB_OK && arrayP != NULL)
    {
      bool partial = (corNgsild.dropV != NULL) || (corNgsild.keepV != NULL);

      for (KjNode* eP = arrayP->value.firstChildP; eP != NULL; eP = eP->next)
      {
        KjNode* idP = kjLookup(eP, "id");
        if (idP == NULL || idP->type != KjString) continue;
        const char* entityId = idP->value.s;

        if (partial)
        {
          int pr = partialPurge(tenantP, entityId, corNgsild.dropV, corNgsild.keepV);
          if (pr == DB_OK)
            kjChildAdd(successArrayP, kjString(corRest.kjsonP, NULL, entityId));
          else if (pr != DB_NOT_FOUND)
            ldDistOpBatchErrorAdd(errorsArrayP, entityId, 500,
                                  LD_ERROR_INTERNAL_ERROR, "Internal Error",
                                  "partial-purge db error", NULL);
        }
        else
        {
          // Pre-fetch for subscription notification
          KjNode* preImage = NULL;
          if (tenantP->subCacheP != NULL)
            db.entityRetrieve(tenantP, entityId, &preImage);

          int dr = db.entityDelete(tenantP, entityId);
          if (dr == DB_OK)
          {
            kjChildAdd(successArrayP, kjString(corRest.kjsonP, NULL, entityId));
            if (tenantP->subCacheP != NULL && preImage != NULL)
              ldNotifyDeferDelete((LdSubCache*) tenantP->subCacheP, preImage, corRest.requestStartTime);
          }
          else if (dr != DB_NOT_FOUND)
          {
            ldDistOpBatchErrorAdd(errorsArrayP, entityId, 500,
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
    corRest.out.httpStatusCode = 204;
    return true;
  }

  KjNode* respBodyP = kjObject(corRest.kjsonP, NULL);
  kjChildAdd(respBodyP, successArrayP);
  kjChildAdd(respBodyP, errorsArrayP);

  corRest.out.responseTree   = respBodyP;
  corRest.out.httpStatusCode = 207;
  return true;
}

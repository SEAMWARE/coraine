//
// FILE            ldSnapshotExecTemporal.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Temporal-snapshot capture pipeline — see header.
//
#include <stdbool.h>                                     // bool
#include <string.h>                                      // strcmp, memset, strlen

#include "corRest/CorRestState.h"                          // corRest

#include "kalloc/kaAlloc.h"                              // kaAlloc
#include "kjson/KjNode.h"                                // KjNode
#include "kjson/kjLookup.h"                              // kjLookup
#include "kjson/kjBuilder.h"                             // kjArray, kjObject, kjString, kjChildAdd, kjChildRemove
#include "kjson/kjFree.h"                                // kjFree

#include "corJsonld/corLdExpand.h"                         // corLdExpand, corLdAlreadyExpanded

#include "corNgsild/corNgsild.h"                           // corNgsild
#include "corNgsild/ldQParse.h"                           // ldQParse, LdQNode
#include "corNgsild/LdSnapshotCache.h"                    // LdSnapshotCache*

#include "troe/TroeDriver.h"                             // troe, TroeQueryFilter, TroeRangeInfo, TROE_OK
#include "troe/troeQTreeToSql.h"                         // troeQTreeToSql

#include "db/Tenant.h"                                   // Tenant

#include "serviceRoutines/ldSnapshotExecTemporal.h"      // Own interface


//
// expandedTypeOrSelf - expand a short type name via the user @context, or
// pass through if already an absolute IRI / NULL.
//
static const char* expandedTypeOrSelf(const char* shortName)
{
  if (shortName == NULL) return NULL;
  if (corLdAlreadyExpanded(shortName)) return shortName;
  return corLdExpand(corNgsild.contextP, shortName, &corRest.kalloc, NULL, NULL);
}



//
// queryToTroeFilter - render a single Query (§ 5.2.23 with temporalQ) into
// a TroeQueryFilter. Allocations are in corRest.kalloc — request-scoped
// (or worker-scoped in async mode); fields are valid for the lifetime of
// the capture call.
//
static bool queryToTroeFilter(KjNode* queryP, TroeQueryFilter* fP)
{
  memset(fP, 0, sizeof(*fP));

  KjNode* tqP = kjLookup(queryP, "temporalQ");
  if (tqP == NULL || tqP->type != KjObject)
    return false;  // § 5.2.23: snapshotTemporalQueries entries must have temporalQ

  KjNode* timerelP    = kjLookup(tqP, "timerel");
  KjNode* timeAtP     = kjLookup(tqP, "timeAt");
  KjNode* endTimeAtP  = kjLookup(tqP, "endTimeAt");
  KjNode* tpropP      = kjLookup(tqP, "timeproperty");
  KjNode* lastNP      = kjLookup(tqP, "lastN");

  if (timerelP == NULL || timerelP->type != KjString) return false;
  if (timeAtP  == NULL || timeAtP->type  != KjString) return false;
  if (strcmp(timerelP->value.s, "between") == 0)
  {
    if (endTimeAtP == NULL || endTimeAtP->type != KjString) return false;
  }

  fP->timerel      = timerelP->value.s;
  fP->timeAtIso    = timeAtP->value.s;
  fP->endTimeAtIso = (endTimeAtP != NULL && endTimeAtP->type == KjString) ? endTimeAtP->value.s : NULL;
  fP->timeproperty = (tpropP     != NULL && tpropP->type     == KjString) ? tpropP->value.s     : NULL;
  fP->lastN        = (lastNP     != NULL && lastNP->type     == KjInt)    ? (int) lastNP->value.i : 0;

  // Entity selectors — flatten id/idPattern/type from the entities array.
  KjNode* entitiesP = kjLookup(queryP, "entities");
  if (entitiesP != NULL && entitiesP->type == KjArray)
  {
    int idCap = 0, typeCap = 0;
    for (KjNode* selP = entitiesP->value.firstChildP; selP != NULL; selP = selP->next)
    {
      if (selP->type != KjObject) continue;
      KjNode* idP = kjLookup(selP, "id");
      if (idP != NULL)
      {
        if      (idP->type == KjString) idCap++;
        else if (idP->type == KjArray)
          for (KjNode* p = idP->value.firstChildP; p != NULL; p = p->next) idCap++;
      }
      if (kjLookup(selP, "type") != NULL) typeCap++;
    }

    char** idV   = (idCap   > 0) ? (char**) kaAlloc(&corRest.kalloc, (idCap   + 1) * sizeof(char*)) : NULL;
    char** typeV = (typeCap > 0) ? (char**) kaAlloc(&corRest.kalloc, (typeCap + 1) * sizeof(char*)) : NULL;
    int    nId = 0, nType = 0;
    const char* idPattern = NULL;

    for (KjNode* selP = entitiesP->value.firstChildP; selP != NULL; selP = selP->next)
    {
      if (selP->type != KjObject) continue;

      KjNode* idP = kjLookup(selP, "id");
      if (idP != NULL)
      {
        if (idP->type == KjString) idV[nId++] = idP->value.s;
        else if (idP->type == KjArray)
          for (KjNode* p = idP->value.firstChildP; p != NULL; p = p->next)
            if (p->type == KjString) idV[nId++] = p->value.s;
      }
      KjNode* idPatP = kjLookup(selP, "idPattern");
      if (idPatP != NULL && idPatP->type == KjString && idPattern == NULL)
        idPattern = idPatP->value.s;
      KjNode* typeP = kjLookup(selP, "type");
      if (typeP != NULL && typeP->type == KjString)
      {
        const char* expanded = expandedTypeOrSelf(typeP->value.s);
        if (expanded != NULL) typeV[nType++] = (char*) expanded;
      }
    }

    if (idV   != NULL) idV[nId]     = NULL;
    if (typeV != NULL) typeV[nType] = NULL;

    fP->idV       = (nId   > 0) ? idV   : NULL;
    fP->typeV     = (nType > 0) ? typeV : NULL;
    fP->idPattern = (char*) idPattern;
  }

  // q-filter — compile to SQL EXISTS predicate via the same helper the
  // live temporal-query path uses.
  KjNode* qP = kjLookup(queryP, "q");
  if (qP != NULL && qP->type == KjString)
  {
    LdQNode* qExpr = ldQParse(qP->value.s, &corRest.kalloc);
    if (qExpr != NULL)
      fP->qSqlPredicate = troeQTreeToSql(qExpr, &corRest.kalloc);
  }

  return true;
}



//
// runOneTemporalQuery - run a single snapshotTemporalQueries entry,
// streaming results into the snap-tenant's TRoE store.
//
// Returns:
//   > 0  : number of entities captured
//   = 0  : query ran but yielded no entities ("empty")
//   < 0  : query failed
//
static int runOneTemporalQuery(LdSnapshotCacheItem* itemP, KjNode* queryP, Tenant* tenantP)
{
  if (itemP->snapTenantP == NULL) return -1;
  Tenant* snapTenantP = (Tenant*) itemP->snapTenantP;

  TroeQueryFilter filter;
  if (!queryToTroeFilter(queryP, &filter))
    return -1;

  TroeRangeInfo rangeInfo;
  memset(&rangeInfo, 0, sizeof(rangeInfo));

  KjNode* result = NULL;
  int     r      = troe.entityTemporalQuery(tenantP, &filter, &result, &rangeInfo);
  if (r != TROE_OK) return -1;
  if (result == NULL || result->type != KjArray || result->value.firstChildP == NULL)
    return 0;

  int n = 0;
  for (KjNode* entityP = result->value.firstChildP; entityP != NULL; entityP = entityP->next)
  {
    if (entityP->type != KjObject) continue;
    if (troe.entityTemporalCreate(snapTenantP, entityP) == TROE_OK)
      n++;
  }
  return n;
}



//
// pickStatus / statusFromString - aggregated outcome per § 5.16.1.4.
// Local copies (not exposed) — same semantics as ldSnapshotExec's
// versions; keeping them duplicated avoids a public helper just for two
// 5-line functions.
//
static const char* pickStatus(int nSuccess, int nEmpty, int nFailure)
{
  if (nSuccess + nEmpty + nFailure == 0) return "failure";
  if (nSuccess > 0 && nFailure == 0 && nEmpty == 0) return "success";
  if (nSuccess > 0)                                  return "partial";
  if (nFailure == 0 && nEmpty > 0)                   return "empty";
  return "failure";
}

static LdSnapshotStatus statusFromString(const char* s)
{
  if (strcmp(s, "success") == 0) return LdSnapshotSuccess;
  if (strcmp(s, "partial") == 0) return LdSnapshotPartial;
  if (strcmp(s, "empty")   == 0) return LdSnapshotEmpty;
  return LdSnapshotFailure;
}



//
// countDetails - tally success / empty / failure rows in a Details
// array on itemP->tree. Used to re-derive snapshotStatus across BOTH
// current-state and temporal details after capture.
//
static void countDetails(KjNode* detailsP, int* nSuccessP, int* nEmptyP, int* nFailureP)
{
  if (detailsP == NULL || detailsP->type != KjArray) return;
  for (KjNode* d = detailsP->value.firstChildP; d != NULL; d = d->next)
  {
    KjNode* sP = kjLookup(d, "resultStatus");
    if (sP == NULL || sP->type != KjString) continue;
    if      (strcmp(sP->value.s, "success") == 0) (*nSuccessP)++;
    else if (strcmp(sP->value.s, "empty")   == 0) (*nEmptyP)++;
    else if (strcmp(sP->value.s, "failure") == 0) (*nFailureP)++;
  }
}



bool ldSnapshotExecTemporalQueries(LdSnapshotCache*     cacheP,
                                   LdSnapshotCacheItem* itemP,
                                   Tenant*              tenantP)
{
  (void) cacheP;
  if (itemP == NULL || itemP->tree == NULL) return false;

  KjNode* qListP = kjLookup(itemP->tree, "snapshotTemporalQueries");
  if (qListP == NULL || qListP->type != KjArray || qListP->value.firstChildP == NULL)
    return true;  // nothing to do — current-state status (if any) stands

  // Plugin guard. Without entityTemporalQuery+Create this is a no-op
  // capture; mark all temporal queries as "failure" so the client
  // sees the gap rather than a misleading "empty".
  bool plugged = (troe.entityTemporalQuery != NULL && troe.entityTemporalCreate != NULL);

  KjNode* detailsP = kjArray(NULL, "snapshotTemporalQueriesDetails");

  for (KjNode* queryP = qListP->value.firstChildP; queryP != NULL; queryP = queryP->next)
  {
    KjNode* detail = kjObject(NULL, NULL);
    const char* result;

    if (!plugged)
      result = "failure";
    else
    {
      int n = runOneTemporalQuery(itemP, queryP, tenantP);
      if      (n  > 0) result = "success";
      else if (n == 0) result = "empty";
      else             result = "failure";
    }

    kjChildAdd(detail, kjString(NULL, "resultStatus", (char*) result));
    kjChildAdd(detailsP, detail);
  }

  // Append snapshotTemporalQueriesDetails to itemP->tree.
  KjNode* existing = kjLookup(itemP->tree, "snapshotTemporalQueriesDetails");
  if (existing != NULL)
  {
    // all-malloc clone — kjChildRemove only unlinks, so free the old details.
    kjChildRemove(itemP->tree, existing);
    kjFree(existing);
  }
  kjChildAdd(itemP->tree, detailsP);

  // Re-derive snapshotStatus from BOTH detail lists.
  int nSuccess = 0, nEmpty = 0, nFailure = 0;
  countDetails(kjLookup(itemP->tree, "snapshotQueriesDetails"),         &nSuccess, &nEmpty, &nFailure);
  countDetails(kjLookup(itemP->tree, "snapshotTemporalQueriesDetails"), &nSuccess, &nEmpty, &nFailure);

  const char* status = pickStatus(nSuccess, nEmpty, nFailure);
  KjNode* sCachedP = kjLookup(itemP->tree, "snapshotStatus");
  if (sCachedP != NULL && sCachedP->type == KjString)
    sCachedP->value.s = (char*) status;
  itemP->status = statusFromString(status);

  return true;
}

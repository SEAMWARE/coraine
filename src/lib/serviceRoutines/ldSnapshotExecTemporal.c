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

#include "corAlloc/corAlloc.h"                           // corAlloc
#include "corTree/CorNode.h"                             // CorNode
#include "corTree/corTreeLookup.h"                       // corTreeLookup
#include "corTree/corTreeBuilder.h"                      // corTreeArray, corTreeObject, corTreeString, corTreeChildAdd, corTreeChildRemove
#include "corTree/corTreeFree.h"                         // corTreeFree

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
static bool queryToTroeFilter(CorNode* queryP, TroeQueryFilter* fP)
{
  memset(fP, 0, sizeof(*fP));

  CorNode* tqP = corTreeLookup(queryP, "temporalQ");
  if (tqP == NULL || tqP->type != CorObject)
    return false;  // § 5.2.23: snapshotTemporalQueries entries must have temporalQ

  CorNode* timerelP   = corTreeLookup(tqP, "timerel");
  CorNode* timeAtP    = corTreeLookup(tqP, "timeAt");
  CorNode* endTimeAtP = corTreeLookup(tqP, "endTimeAt");
  CorNode* tpropP     = corTreeLookup(tqP, "timeproperty");
  CorNode* lastNP     = corTreeLookup(tqP, "lastN");

  if (timerelP == NULL || timerelP->type != CorString) return false;
  if (timeAtP  == NULL || timeAtP->type  != CorString) return false;
  if (strcmp(timerelP->value.s, "between") == 0)
  {
    if (endTimeAtP == NULL || endTimeAtP->type != CorString) return false;
  }

  fP->timerel      = timerelP->value.s;
  fP->timeAtIso    = timeAtP->value.s;
  fP->endTimeAtIso = (endTimeAtP != NULL && endTimeAtP->type == CorString) ? endTimeAtP->value.s : NULL;
  fP->timeproperty = (tpropP     != NULL && tpropP->type     == CorString) ? tpropP->value.s    : NULL;
  fP->lastN        = (lastNP     != NULL && lastNP->type     == CorInt)   ? (int) lastNP->value.i : 0;

  // Entity selectors — flatten id/idPattern/type from the entities array.
  CorNode* entitiesP = corTreeLookup(queryP, "entities");
  if (entitiesP != NULL && entitiesP->type == CorArray)
  {
    int idCap = 0, typeCap = 0;
    for (CorNode* selP = entitiesP->value.head; selP != NULL; selP = selP->next)
    {
      if (selP->type != CorObject) continue;
      CorNode* idP = corTreeLookup(selP, "id");
      if (idP != NULL)
      {
        if      (idP->type == CorString) idCap++;
        else if (idP->type == CorArray)
          for (CorNode* p = idP->value.head; p != NULL; p = p->next) idCap++;
      }
      if (corTreeLookup(selP, "type") != NULL) typeCap++;
    }

    char** idV   = (idCap   > 0) ? (char**) corAlloc(&corRest.kalloc, (idCap  + 1) * sizeof(char*)) : NULL;
    char** typeV = (typeCap > 0) ? (char**) corAlloc(&corRest.kalloc, (typeCap + 1) * sizeof(char*)) : NULL;
    int    nId = 0, nType = 0;
    const char* idPattern = NULL;

    for (CorNode* selP = entitiesP->value.head; selP != NULL; selP = selP->next)
    {
      if (selP->type != CorObject) continue;

      CorNode* idP = corTreeLookup(selP, "id");
      if (idP != NULL)
      {
        if (idP->type == CorString) idV[nId++] = idP->value.s;
        else if (idP->type == CorArray)
          for (CorNode* p = idP->value.head; p != NULL; p = p->next)
            if (p->type == CorString) idV[nId++] = p->value.s;
      }
      CorNode* idPatP = corTreeLookup(selP, "idPattern");
      if (idPatP != NULL && idPatP->type == CorString && idPattern == NULL)
        idPattern = idPatP->value.s;
      CorNode* typeP = corTreeLookup(selP, "type");
      if (typeP != NULL && typeP->type == CorString)
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
  CorNode* qP = corTreeLookup(queryP, "q");
  if (qP != NULL && qP->type == CorString)
  {
    LdQNode* qExpr = ldQParse(qP->value.s, &corRest.kalloc);
    if (qExpr != NULL)
    {
      fP->qSqlPredicate = troeQTreeToSql(qExpr, &corRest.kalloc);
      fP->qTree         = qExpr;
    }
  }

  return true;
}



//
// instanceIdsRemove - an instanceId is assigned by the store that keeps the
// instance; the snapshot's store assigns its own. One carried over from the
// source is not the snapshot's - a TRoE plugin keeps it as a sub-attribute,
// beside the instanceId it assigns.
//
static void instanceIdsRemove(CorNode* entityP)
{
  for (CorNode* attrP = entityP->value.head; attrP != NULL; attrP = attrP->next)
  {
    if      (attrP->type == CorObject)
    {
      CorNode* idP = corTreeLookup(attrP, "instanceId");
      if (idP != NULL) corTreeChildRemove(attrP, idP);
    }
    else if (attrP->type == CorArray)
    {
      for (CorNode* instP = attrP->value.head; instP != NULL; instP = instP->next)
      {
        if (instP->type != CorObject) continue;
        CorNode* idP = corTreeLookup(instP, "instanceId");
        if (idP != NULL) corTreeChildRemove(instP, idP);
      }
    }
  }
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
static int runOneTemporalQuery(LdSnapshotCacheItem* itemP, CorNode* queryP, Tenant* tenantP)
{
  if (itemP->snapTenantP == NULL) return -1;
  Tenant* snapTenantP = (Tenant*) itemP->snapTenantP;

  TroeQueryFilter filter;
  if (!queryToTroeFilter(queryP, &filter))
    return -1;

  TroeRangeInfo rangeInfo;
  memset(&rangeInfo, 0, sizeof(rangeInfo));

  CorNode* result = NULL;
  int     r      = troe.entityTemporalQuery(tenantP, &filter, &result, &rangeInfo);
  if (r != TROE_OK) return -1;
  if (result == NULL || result->type != CorArray || result->value.head == NULL)
    return 0;

  int n = 0;
  for (CorNode* entityP = result->value.head; entityP != NULL; entityP = entityP->next)
  {
    if (entityP->type != CorObject) continue;
    instanceIdsRemove(entityP);
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
static void countDetails(CorNode* detailsP, int* nSuccessP, int* nEmptyP, int* nFailureP)
{
  if (detailsP == NULL || detailsP->type != CorArray) return;
  for (CorNode* d = detailsP->value.head; d != NULL; d = d->next)
  {
    CorNode* sP = corTreeLookup(d, "resultStatus");
    if (sP == NULL || sP->type != CorString) continue;
    if      (strcmp(sP->value.s, "success") == 0) (*nSuccessP)++;
    else if (strcmp(sP->value.s, "empty")   == 0) (*nEmptyP)++;
    else if (strcmp(sP->value.s, "failure") == 0) (*nFailureP)++;
  }
}



bool ldSnapshotExecTemporalQueries(LdSnapshotCache*     cacheP,
                                   LdSnapshotCacheItem* itemP,
                                   Tenant*              tenantP)
{
  if (itemP == NULL || itemP->tree == NULL) return false;

  // As ldSnapshotExecQueries: lookup under the rdlock (the list itself is immutable), the
  // grafting and the status update under the wrlock
  ldSnapshotCacheRdLock(cacheP);
  CorNode* qListP = corTreeLookup(itemP->tree, "snapshotTemporalQueries");
  ldSnapshotCacheUnlock(cacheP);
  if (qListP == NULL || qListP->type != CorArray || qListP->value.head == NULL)
    return true;  // nothing to do — current-state status (if any) stands

  // Plugin guard. Without entityTemporalQuery+Create this is a no-op
  // capture; mark all temporal queries as "failure" so the client
  // sees the gap rather than a misleading "empty".
  bool plugged = (troe.entityTemporalQuery != NULL && troe.entityTemporalCreate != NULL);

  CorNode* detailsP = corTreeArray(NULL, "snapshotTemporalQueriesDetails");

  for (CorNode* queryP = qListP->value.head; queryP != NULL; queryP = queryP->next)
  {
    CorNode* detail = corTreeObject(NULL, NULL);
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

    corTreeChildAdd(detail, corTreeString(NULL, "resultStatus", (char*) result));
    corTreeChildAdd(detailsP, detail);
  }

  ldSnapshotCacheWrLock(cacheP);

  // Append snapshotTemporalQueriesDetails to itemP->tree.
  CorNode* existing = corTreeLookup(itemP->tree, "snapshotTemporalQueriesDetails");
  if (existing != NULL)
  {
    // all-malloc clone — corTreeChildRemove only unlinks, so free the old details.
    corTreeChildRemove(itemP->tree, existing);
    corTreeFree(existing);
  }
  corTreeChildAdd(itemP->tree, detailsP);

  // Re-derive snapshotStatus from BOTH detail lists.
  int nSuccess = 0, nEmpty = 0, nFailure = 0;
  countDetails(corTreeLookup(itemP->tree, "snapshotQueriesDetails"),    &nSuccess, &nEmpty, &nFailure);
  countDetails(corTreeLookup(itemP->tree, "snapshotTemporalQueriesDetails"), &nSuccess, &nEmpty, &nFailure);

  const char* status = pickStatus(nSuccess, nEmpty, nFailure);
  CorNode* sCachedP = corTreeLookup(itemP->tree, "snapshotStatus");
  if (sCachedP != NULL && sCachedP->type == CorString)
    sCachedP->value.s = (char*) status;
  itemP->status = statusFromString(status);

  ldSnapshotCacheUnlock(cacheP);

  return true;
}

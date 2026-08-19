//
// FILE            purgeSnapshots.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// DELETE /ngsi-ld/v1/snapshots?q=... — Purge Snapshots (§ 16.7).
//
// The q-filter (NGSI-LD § 4.9) matches against members of the Snapshot
// data type — typically snapshotStatus, snapshotPriority, expiresAt,
// lastUsedAt. Snapshots whose stored tree satisfies the q-expression
// are removed from the cache.
//
// q is MANDATORY (§ 16.7.4) — a missing or empty q is a BadRequestData,
// not a licence to purge the whole tenant.
//
#include <stdbool.h>                                     // bool
#include <string.h>                                      // strcmp

#include "corRest/CorRestState.h"                          // corRest

#include "kalloc/kaAlloc.h"                              // kaAlloc
#include "kalloc/kaStrdup.h"                             // kaStrdup
#include "kjson/KjNode.h"                                // KjNode

#include "kjson/kjLookup.h"                              // kjLookup

#include "corNgsild/corNgsild.h"                           // ldError, corNgsild
#include "corNgsild/LdProblem.h"                          // LD_ERROR_*
#include "corNgsild/LdQ.h"                                // LdQNode, LdQTerm
#include "corNgsild/ldQParse.h"                           // ldQParse
#include "corNgsild/LdSnapshotCache.h"                    // LdSnapshotCache, ldSnapshotCacheItemDelete
#include "corNgsild/ldSnapshotNotify.h"                   // ldSnapshotNotify

#include "db/DbDriver.h"                                 // db
#include "db/Tenant.h"                                   // Tenant
#include "db/snapshotTenant.h"                           // snapshotTenantDestroy
#include "troe/TroeDriver.h"                             // troe

#include "serviceRoutines/purgeSnapshots.h"              // Own interface


//
// shortNameFromIri - last path segment of an expanded IRI. The q-parser
// expands attr names against the request context (core context here),
// so "snapshotPriority" arrives as "https://uri.etsi.org/ngsi-ld/snapshotPriority".
// Snapshot trees store members by their short JSON-LD name, so we
// match by the trailing segment to bridge the gap. Plain short names
// (no '/' or ':') pass through unchanged.
//
static const char* shortNameFromIri(const char* iri)
{
  if (iri == NULL) return NULL;
  const char* last = iri;
  for (const char* p = iri; *p != 0; p++)
    if (*p == '/' || *p == ':') last = p + 1;
  return last;
}



//
// nodeAsDouble - read a numeric value from a snapshot field.
//
static bool nodeAsDouble(KjNode* nodeP, double* outP)
{
  if      (nodeP->type == KjInt)   { *outP = (double) nodeP->value.i; return true; }
  else if (nodeP->type == KjFloat) { *outP = nodeP->value.f;          return true; }
  return false;
}



//
// matchTerm - evaluate one LdQTerm against a flat snapshot tree.
//
// Snapshot members are plain JSON values (not entity-storage wrapped),
// so the comparison logic is the simple flavour: lookup by short name,
// type-check, compare. Existence and the common comparators are
// supported; LdQRange/LdQValueList/LdQPattern are kept since the spec
// allows them but they're rarely useful for snapshot members.
//
static bool matchTerm(KjNode* tree, LdQTerm* termP)
{
  const char* name  = shortNameFromIri(termP->attr);
  KjNode*     fldP  = kjLookup(tree, name);

  if (termP->op == LdQExists)
    return fldP != NULL;
  if (termP->op == LdQNotExists)
    return fldP == NULL;

  if (fldP == NULL) return false;

  if (termP->valueType == LdQNumber)
  {
    double f;
    if (!nodeAsDouble(fldP, &f)) return false;
    double v = termP->value.n;
    switch (termP->op)
    {
      case LdQEqual:     return f == v;
      case LdQUnequal:   return f != v;
      case LdQGreater:   return f >  v;
      case LdQLess:      return f <  v;
      case LdQGreaterEq: return f >= v;
      case LdQLessEq:    return f <= v;
      default:           return false;
    }
  }

  if (termP->valueType == LdQString && fldP->type == KjString)
  {
    int cmp = strcmp(fldP->value.s, termP->value.s);
    switch (termP->op)
    {
      case LdQEqual:     return cmp == 0;
      case LdQUnequal:   return cmp != 0;
      case LdQGreater:   return cmp >  0;
      case LdQLess:      return cmp <  0;
      case LdQGreaterEq: return cmp >= 0;
      case LdQLessEq:    return cmp <= 0;
      default:           return false;
    }
  }

  if (termP->valueType == LdQBool && fldP->type == KjBoolean)
  {
    bool v = termP->value.b;
    bool f = (fldP->value.i != 0);
    return (termP->op == LdQEqual) ? (f == v) : (f != v);
  }

  return false;
}



static bool matchSnapshot(KjNode* tree, LdQNode* nodeP)
{
  if (nodeP == NULL || tree == NULL) return false;

  switch (nodeP->type)
  {
    case LdQTermNode:
      return matchTerm(tree, &nodeP->term);

    case LdQAndNode:
      for (int i = 0; i < nodeP->group.count; i++)
        if (!matchSnapshot(tree, nodeP->group.childV[i])) return false;
      return true;

    case LdQOrNode:
      for (int i = 0; i < nodeP->group.count; i++)
        if (matchSnapshot(tree, nodeP->group.childV[i])) return true;
      return false;

    case LdQLinkedNode:
      // No relationship semantics in a snapshot tree.
      return false;
  }
  return false;
}



//
// readQParam - find ?q=... in the parsed URL params (no decoding here;
// the framework already URL-decoded the value).
//
static const char* readQParam(void)
{
  for (int i = 0; i < corRest.in.uriParamCount; i++)
  {
    if (strcmp(corRest.in.uriParamV[i].key, "q") == 0)
      return corRest.in.uriParamV[i].value;
  }
  return NULL;
}



bool purgeSnapshots(void)
{
  Tenant* tenantP = (Tenant*) corNgsild.tenantP;

  //
  // § 16.7.4: "If the NGSI-LD Query is not present or it is not a valid as per
  // clause 7.2.3, restricted to members of the Snapshot data type, then an error
  // of type BadRequestData shall be raised."
  //
  // So the query is mandatory: an unqualified DELETE on the collection must not
  // wipe every snapshot on the tenant. Purging all of them is still possible,
  // with a q that matches all - it just has to be asked for explicitly.
  //
  // Checked before the empty-cache shortcut below, so a missing q is reported
  // the same way whether or not the tenant happens to hold any snapshots.
  //
  const char* qStr = readQParam();
  if ((qStr == NULL) || (qStr[0] == 0))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Mandatory URI Parameter Missing",
            "Purge Snapshots requires a 'q' query selecting the snapshots to purge");
    return true;
  }

  if (tenantP->snapshotCacheP == NULL)
  {
    corRest.out.httpStatusCode = 204;
    return true;
  }

  LdSnapshotCache* cacheP = (LdSnapshotCache*) tenantP->snapshotCacheP;

  LdQNode* qP = ldQParse(qStr, &corRest.kalloc);
  if (qP == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid q-expression",
            "invalid q expression: '%s'", qStr);
    return true;
  }

  //
  // Walk the snapshot list. Collect ids of matching snapshots first to
  // avoid mutating the list mid-iteration. Cap at the current count —
  // the cache is bounded.
  //
  int cap = cacheP->count;
  const char** victims = NULL;
  int          n       = 0;
  if (cap > 0)
    victims = (const char**) kaAlloc(&corRest.kalloc, cap * sizeof(char*));

  for (LdSnapshotCacheItem* p = cacheP->head; p != NULL; p = p->next)
  {
    bool match = (qP == NULL) ? true : matchSnapshot(p->tree, qP);
    // p->id points INTO p->tree, which ldSnapshotCacheItemDelete frees below —
    // copy it into the request arena so it stays valid across the delete loop
    // (db.snapshotDelete still needs the id after the item is gone).
    if (match && n < cap)
      victims[n++] = kaStrdup(&corRest.kalloc, p->id);
  }

  for (int i = 0; i < n; i++)
  {
    LdSnapshotCacheItem* victimP = ldSnapshotCacheItemLookup(cacheP, victims[i]);
    Tenant*              snapP   = (victimP != NULL) ? (Tenant*) victimP->snapTenantP : NULL;

    // § 5.16.6 — deletion notification BEFORE the item is unlinked.
    if (victimP != NULL) ldSnapshotNotify(victimP, true);

    ldSnapshotCacheItemDelete(cacheP, victims[i]);

    if (db.snapshotDelete != NULL)
      db.snapshotDelete(tenantP, victims[i]);
    if (troe.tenantDrop != NULL && snapP != NULL)
      troe.tenantDrop(snapP);
    if (db.tenantDrop != NULL && snapP != NULL)
      db.tenantDrop(snapP);
    snapshotTenantDestroy(snapP);
  }

  corRest.out.httpStatusCode = 204;
  return true;
}

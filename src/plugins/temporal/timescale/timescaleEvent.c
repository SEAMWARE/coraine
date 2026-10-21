//
// FILE            timescaleEvent.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Write path for the timescale TRoE plugin. Walks the attrSnapshot to
// extract typed values into the v_text / v_number / v_bool / v_compound
// columns and the sub_attrs JSONB. The rows of an event list are rendered
// into one unit (timescaleUnit.h) and handed to the tenant's TRoE writers
// (timescaleWriter.h), which record the units of concurrent requests in
// shared transactions.
//

#include <stdbool.h>                                      // bool
#include <stddef.h>                                       // NULL
#include <stdio.h>                                        // snprintf
#include <string.h>                                       // strcmp
#include <time.h>                                         // gmtime_r, time_t
#include <libpq-fe.h>                                     // PG*

#include "corLog/corLog.h"                                // COR_E

#include "corTree/CorNode.h"                              // CorNode
#include "corTree/corTreeLookup.h"                        // corTreeLookup
#include "corTree/corTreeBuilder.h"                       // corTreeObject, corTreeChildAdd
#include "corTree/corTreeClone.h"                         // corTreeClone
#include "corJson/corJsonRender.h"                        // corJsonFastRender
#include "corJson/corJsonRenderSize.h"                    // corJsonFastRenderSize
#include "corAlloc/corAlloc.h"                            // corAlloc
#include "corAlloc/corAllocStrdup.h"                      // corAllocStrdup
#include "corAlloc/CorAlloc.h"                            // CorAlloc

#include "corRest/CorRestState.h"                           // corRest
#include "corNgsild/LdAttrType.h"                          // LdAttr*
#include "corNgsild/ldAttrTypeDetect.h"                    // ldAttrTypeDetect
#include "corNgsild/ldTermId.h"                            // ldTermId, CorTerm*

#include "troe/TroeDriver.h"                              // TroeEvent, TroeOp*

#include "temporal/timescale/timescaleGlobals.h"          // timescaleConn
#include "temporal/timescale/timescalePool.h"             // timescaleConnGet, timescaleConnRelease
#include "temporal/timescale/timescaleUnit.h"             // TsUnit, tsBuf*, timescaleUnitsExec
#include "temporal/timescale/timescaleWriter.h"           // timescaleWriterSubmit
#include "temporal/timescale/timescaleEvent.h"            // Own interface



// -----------------------------------------------------------------------------
//
// opName - map TroeOp → SQL string for the op column.
//
static const char* opName(TroeOp op)
{
  switch (op)
  {
    case TroeOpEntityCreated:  return "created";
    case TroeOpEntityReplaced: return "replaced";
    case TroeOpEntityDeleted:  return "deleted";
    case TroeOpAttrCreated:    return "created";
    case TroeOpAttrModified:   return "modified";
    case TroeOpAttrReplaced:   return "replaced";
    case TroeOpAttrDeleted:    return "deleted";
  }
  return "unknown";
}



// -----------------------------------------------------------------------------
//
// timescaleNsToSqlTimestamp - epoch nanoseconds → "to_timestamp(<seconds>)"
// SQL fragment. Exposed so timescaleHistoryWrite can format the broker's
// request-start timestamp into UPDATE statements without duplicating the
// conversion.
//
void timescaleNsToSqlTimestamp(uint64_t ns, char* buf, int bufSize)
{
  double secs = (double) ns / 1e9;
  snprintf(buf, bufSize, "to_timestamp(%.6f)", secs);
}




// -----------------------------------------------------------------------------
//
// AttrCols - column values extracted from one attribute snapshot.
//
// All `const char*` pointers — NULL means "DON'T BIND" (column stays
// NULL in SQL via DEFAULT). Strings live in corRest.kalloc, no free.
//
typedef struct
{
  int          kind;                // 0..8 — LdAttrType cast to int
  const char*  dsId;                // dataset_id ("" for default-instance)
  const char*  observedAtIso;       // ISO string passed straight to postgres
  const char*  v_text;
  const char*  v_number;
  const char*  v_bool;
  const char*  v_datetime;
  const char*  v_compound;          // JSONB text
  const char*  sub_attrs;           // JSONB text
} AttrCols;



// -----------------------------------------------------------------------------
//
// numberToText - render a number node as a decimal text string for v_number.
//
static const char* numberToText(CorNode* nP, CorAlloc* allocP)
{
  char buf[64];
  if (nP->type == CorInt)
    snprintf(buf, sizeof(buf), "%lld", (long long) nP->value.i);
  else
    snprintf(buf, sizeof(buf), "%.17g", nP->value.f);
  return corAllocStrdup(allocP, buf);
}



// -----------------------------------------------------------------------------
//
// renderJsonb - render a CorNode subtree as JSON text suitable for ::jsonb.
//
static const char* renderJsonb(CorNode* nP, CorAlloc* allocP)
{
  int   sz  = corJsonFastRenderSize(nP) + 1;
  char* buf = (char*) corAlloc(allocP, sz);
  corJsonFastRender(nP, buf);
  return buf;
}



// -----------------------------------------------------------------------------
//
// extractCols - walk an attr DB-format snapshot and fill the AttrCols.
//
// Snapshot shape (DB-model, post ldApiEntityToDbModel):
//   {
//     "@none":     { "type": "Property", "value": 42, "observedAt": "..." },
//     "urn:ds:1":  { ... }
//   }
//
// For v1 we only emit a row for the first dataset entry. Multi-instance
// expansion lands in a follow-up.
//
static void extractCols(CorNode* attrSnapshot, AttrCols* cP)
{
  memset(cP, 0, sizeof(*cP));
  cP->dsId = "";

  if (attrSnapshot == NULL || attrSnapshot->type != CorObject)
    return;

  // First child = first dataset instance.
  CorNode* instP = attrSnapshot->value.head;
  if (instP == NULL || instP->type != CorObject)
    return;

  if (instP->name != NULL && strcmp(instP->name, "@none") != 0)
    cP->dsId = instP->name;

  // Detect attr-kind. ldAttrTypeDetect inspects the explicit "type" string
  // and falls back to value-shape heuristics.
  cP->kind = (int) ldAttrTypeDetect(instP);

  // The value of an attribute may live under any of these keys depending on
  // its kind:
  //   Property / GeoProperty / VocabProperty / JsonProperty / ListProperty → "value"
  //   Relationship / ListRelationship                                       → "object"
  //   LanguageProperty                                                      → "languageMap"
  // (Note: after JSON-LD expansion via corLdExpandTree these stay as short
  //  names — they're core-context terms with the keep-short shortcut.)
  // ldApiEntityToDbModel normalises all of them to "value" before regular
  // POST /entities; the temporal POST path may feed us API-format directly,
  // so accept either form here.
  CorNode* valueP  = NULL;
  CorNode* observP = NULL;
  CorNode* subAttrs = corTreeObject(corRest.kallocP, NULL);

  // Read-only walk: nothing below re-homes a node out of instP (see the
  // sub-attribute branch), so the list stays whole. nextP is still captured up
  // front - it costs nothing and it is what keeps this loop correct if anyone
  // ever adds a step that does touch the list.
  CorNode* fP = instP->value.head;
  while (fP != NULL)
  {
    CorNode* nextP = fP->next;

    if (fP->name == NULL) { fP = nextP; continue; }

    if (ldTermId(fP) == CorTermType) { fP = nextP; continue; }
    if (ldTermId(fP) == CorTermCreatedAt) { fP = nextP; continue; }
    if (ldTermId(fP) == CorTermModifiedAt) { fP = nextP; continue; }
    if (ldTermId(fP) == CorTermDatasetId) { fP = nextP; continue; }

    if (ldTermId(fP) == CorTermObservedAt)   { observP = fP;       fP = nextP; continue; }
    if (ldTermId(fP) == CorTermValue ||
        ldTermId(fP) == CorTermObject ||
        ldTermId(fP) == CorTermLanguageMap ||
        ldTermId(fP) == CorTermVocab ||
        ldTermId(fP) == CorTermValueList ||
        ldTermId(fP) == CorTermObjectList ||
        ldTermId(fP) == CorTermJson)   { valueP  = fP;       fP = nextP; continue; }

    //
    // Sub-attribute. It belongs to the CALLER's entity tree, so it is CLONED
    // in - neither moved nor, as it was, spliced.
    //
    // corTreeChildAdd re-points the added node's ->next, and there being no
    // corTreeChildMove it left the two lists SHARING their tails: the first
    // sub-attribute's ->next was nulled, then the second sub-attribute relinked
    // it, so {type,value,unitCode,observedAt,accuracy} came back out of here as
    // {type,value,unitCode,accuracy} - whatever sat between two sub-attributes
    // spliced out of the attribute we were handed. observedAt is what sits
    // between them in the most ordinary attribute there is.
    //
    // corTreeChildRemove-then-corTreeChildAdd is the correct idiom for a MOVE and does
    // repair the lists, but a move is not what this wants: the sub-attributes
    // would then be gone from the caller's attribute instead, and the entity
    // would persist without its unitCode. extractCols READS an attribute into
    // columns. It owns none of what it is shown.
    //
    // Cloning the sub-attributes rather than instP as a whole is deliberate:
    // the value may be an arbitrarily large compound and is only read.
    //
    // Under the default deferred dispatch nobody could see it: the event is
    // handed to the plugin from brokerPostResponseHook, long after the entity
    // has been stored and rendered. --troeSync runs the plugin where the
    // service routine produces the event - BEFORE the current-state write - and
    // the truncation reached the database. Eleven ETSI batch TPs went red the
    // first night the conformance broker took the flag.
    //
    // A temporal write READS the entity. It must not consume it.
    //
    corTreeChildAdd(subAttrs, corTreeClone(corRest.kallocP, fP));
    fP = nextP;
  }

  // observedAt:
  //   - CorString: ISO text, pass through.
  //   - CorInt:   epoch nanoseconds (ldApiEntityToDbModel normalises ISO → ns).
  //               Format as fractional-seconds for postgres ::timestamptz cast.
  if (observP != NULL)
  {
    if (observP->type == CorString)
    {
      cP->observedAtIso = observP->value.s;
    }
    else if (observP->type == CorInt)
    {
      char* buf = (char*) corAlloc(&corRest.kalloc, 64);
      double secs = (double) observP->value.i / 1e9;
      // ::timestamptz accepts "epoch" floats via to_timestamp(); for direct
      // cast we want an ISO string. Build it.
      time_t t = (time_t) (observP->value.i / 1000000000LL);
      long   ns = (long) (observP->value.i % 1000000000LL);
      struct tm tmv;
      gmtime_r(&t, &tmv);
      snprintf(buf, 64, "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ",
               tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
               tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ns / 1000);
      cP->observedAtIso = buf;
      (void) secs;
    }
  }

  // Value column selection.
  if (valueP != NULL)
  {
    switch (valueP->type)
    {
      case CorString: cP->v_text     = valueP->value.s; break;
      case CorInt:
      case CorFloat:  cP->v_number   = numberToText(valueP, &corRest.kalloc); break;
      case CorBoolean: cP->v_bool    = valueP->value.b ? "t" : "f"; break;
      case CorObject:
      case CorArray:  cP->v_compound = renderJsonb(valueP, &corRest.kalloc); break;
      case CorNull:   /* leave all NULL */ break;
      default: break;
    }
  }

  // sub_attrs only emitted if non-empty.
  if (subAttrs->value.head != NULL)
    cP->sub_attrs = renderJsonb(subAttrs, &corRest.kalloc);
}




// -----------------------------------------------------------------------------
//
// troeTypeNameQuoted - append "name" to a postgres array literal, quote- and
// backslash-escaped.
//
int troeTypeNameQuoted(const char* name, char* buf, int pos, int bufSize)
{
  if (pos + 3 >= bufSize)
    return pos;

  buf[pos++] = '"';

  for (const char* p = name; (*p != 0) && (pos + 3 < bufSize); p++)
  {
    if ((*p == '"') || (*p == '\\'))
      buf[pos++] = '\\';
    buf[pos++] = *p;
  }

  buf[pos++] = '"';

  return pos;
}




// -----------------------------------------------------------------------------
//
// entityTypeArrayLiteral - render the Entity's type(s) as a postgres text[]
// literal, e.g. {"Vehicle","Car"}.
//
// An Entity carries one type name or several (§ 5.2.6.4.2), and § 11.2.3.4 lets
// the Temporal API grow that list, so troe_entities/troe_attrs.entity_type is a
// TEXT[] (migration 2).
//
// The list is taken from the event's entity snapshot when there is one, because
// TroeEvent.entityType is a single string and the service routines leave it NULL
// for a multi-type Entity - which used to store an empty type in the temporal
// history and lose it. Without a snapshot the single name is all there is.
//
// Type names are IRIs, so a quote or backslash is not expected; escaped anyway
// rather than trusting the input to stay that way.
//
static const char* entityTypeArrayLiteral(const TroeEvent* evP, char* buf, int bufSize)
{
  CorNode* typeP = (evP->entitySnapshot != NULL) ? corTreeLookup(evP->entitySnapshot, "type") : NULL;
  int     pos   = 0;

  buf[pos++] = '{';

  if ((typeP != NULL) && (typeP->type == CorArray))
  {
    bool first = true;

    for (CorNode* tP = typeP->value.head; tP != NULL; tP = tP->next)
    {
      if ((tP->type != CorString) || (tP->value.s == NULL))
        continue;
      if (!first)
        buf[pos++] = ',';
      first = false;
      pos = troeTypeNameQuoted(tP->value.s, buf, pos, bufSize);
    }
  }
  else
  {
    const char* single = NULL;

    if ((typeP != NULL) && (typeP->type == CorString))
      single = typeP->value.s;
    else if (evP->entityType != NULL)
      single = evP->entityType;

    if ((single != NULL) && (single[0] != 0))
      pos = troeTypeNameQuoted(single, buf, pos, bufSize);
  }

  buf[pos++] = '}';
  buf[pos]   = 0;

  return buf;
}



// -----------------------------------------------------------------------------
//
// entityRowAdd - one row of troe_entities, into the unit
//
static void entityRowAdd(TsUnit* uP, const TroeEvent* evP)
{
  char   tsExpr[64];
  char   typeLit[1024];
  TsBuf* bP = &uP->entityRows;
  bool   ok;

  timescaleNsToSqlTimestamp(evP->modifiedAtNs, tsExpr, sizeof(tsExpr));

  ok = tsBufText(bP, "%s(", (uP->entityRowN == 0) ? "" : ",");
  ok = ok && tsBufLiteral(bP, (evP->entityId != NULL) ? evP->entityId : "", NULL);
  ok = ok && tsBufText(bP, ", ");
  ok = ok && tsBufLiteral(bP, entityTypeArrayLiteral(evP, typeLit, sizeof(typeLit)), "text[]");
  ok = ok && tsBufText(bP, ", %s, '%s')", tsExpr, opName(evP->op));

  uP->entityRowN += 1;
  if (ok == false)
    uP->oom = true;
}



// -----------------------------------------------------------------------------
//
// attrRowAdd - one row of troe_attrs per instance, into the unit
//
static void attrRowAdd(TsUnit* uP, const TroeEvent* evP)
{
  AttrCols cols;

  //
  // A snapshot is a wrapper keyed by datasetId - { "@none": {...}, "urn:ds:1": {...} }
  // - and a row is ONE instance. A wrapper with several instances (a create or
  // replace of a multi-instance Attribute, a deleteAll) is a row per instance:
  // each goes through here again, in a one-instance wrapper of its own. The
  // wrapper is a copy on the stack, so no list is touched - only the head
  // is ever read from it.
  //
  CorNode* wrapperP = (CorNode*) evP->attrSnapshot;

  if ((evP->datasetId == NULL) && (wrapperP != NULL) && (wrapperP->type == CorObject) &&
      (wrapperP->value.head != NULL) && (wrapperP->value.head->next != NULL))
  {
    for (CorNode* instP = wrapperP->value.head; instP != NULL; instP = instP->next)
    {
      if (instP->type != CorObject)
        continue;

      CorNode    one   = *wrapperP;
      TroeEvent  oneEv = *evP;

      one.value.head        = instP;
      one.value.tail        = instP;
      oneEv.attrSnapshot    = &one;
      oneEv.datasetId       = ((instP->name != NULL) && (strcmp(instP->name, "@none") != 0)) ? instP->name : "";

      attrRowAdd(uP, &oneEv);
    }

    return;
  }

  if (evP->op == TroeOpAttrDeleted)
  {
    // Tombstone row: no value columns are bound, but attr_kind must survive —
    // the read side derives the instance's "type" and value-field name from
    // it (§ 5.3.2.5: a deleted instance keeps the Attribute's type). For
    // deletes, attrSnapshot is the PRE-delete wrapper ({dsKey: instance}),
    // shared with the notification renderer (showChanges previousValue), so
    // detect the kind without extractCols' destructive sub-attr migration.
    // (For a deleted scope the snapshot is a bare string/array — kind stays
    // 0 and renders as Property, which is what § 5.3.2.5 mandates for scope.)
    memset(&cols, 0, sizeof(cols));
    CorNode* wrapP = (CorNode*) evP->attrSnapshot;
    CorNode* instP = (wrapP != NULL && wrapP->type == CorObject) ? wrapP->value.head : NULL;

    // The kind of the instance the event names, when it names one
    if ((evP->datasetId != NULL) && (wrapP != NULL) && (wrapP->type == CorObject))
    {
      CorNode* namedP = corTreeLookup(wrapP, (evP->datasetId[0] != 0) ? evP->datasetId : "@none");
      if (namedP != NULL)
        instP = namedP;
    }
    if (instP != NULL && instP->type == CorObject)
      cols.kind = (int) ldAttrTypeDetect(instP);
  }
  else
    extractCols((CorNode*) evP->attrSnapshot, &cols);

  cols.dsId = (cols.dsId != NULL) ? cols.dsId : "";

  char tsExpr[64];
  timescaleNsToSqlTimestamp(evP->modifiedAtNs, tsExpr, sizeof(tsExpr));

  //
  // created_at is modified_at - unless the event is an imported instance (coraine-import) that
  // carries a creation time of its own, and an instance id of its own (TroeDriver.h).
  //
  char createdExpr[64];
  timescaleNsToSqlTimestamp((evP->createdAtNs != 0) ? evP->createdAtNs : evP->modifiedAtNs, createdExpr, sizeof(createdExpr));

  const char* entityId = (evP->entityId != NULL) ? evP->entityId : "";
  char        typeLit[1024];
  TsBuf*      bP = &uP->attrRows;
  bool        ok;

  entityTypeArrayLiteral(evP, typeLit, sizeof(typeLit));

  //
  // instance_id: an imported instance brings its own, unique in the store it came from; otherwise
  // the table's DEFAULT generates one - so each row is unique by construction and there is no
  // ON CONFLICT. created_at and modified_at are seeded to the same instant on insert; Modify-Instance
  // later bumps modified_at while leaving created_at fixed.
  //
  ok = tsBufText(bP, "%s(", (uP->attrRowN == 0) ? "" : ",");
  if ((evP->instanceId != NULL) && (evP->instanceId[0] != 0))
    ok = ok && tsBufLiteral(bP, evP->instanceId, NULL);
  else
    ok = ok && tsBufText(bP, "DEFAULT");
  ok = ok && tsBufText(bP, ", ");
  ok = ok && tsBufLiteral(bP, entityId, NULL);
  ok = ok && tsBufText(bP, ", ");

  //
  // An attribute event carries no entity snapshot, so the type can come out empty for a multi-type
  // Entity; the type list already recorded for this Entity then, rather than storing nothing. Only
  // then - the subquery is not paid for by the rows that know their type.
  //
  if (strcmp(typeLit, "{}") == 0)
  {
    ok = ok && tsBufText(bP, "COALESCE((SELECT entity_type FROM troe_entities WHERE entity_id = ");
    ok = ok && tsBufLiteral(bP, entityId, NULL);
    ok = ok && tsBufText(bP, " ORDER BY modified_at DESC LIMIT 1), '{}'::text[])");
  }
  else
    ok = ok && tsBufLiteral(bP, typeLit, "text[]");

  ok = ok && tsBufText(bP, ", ");
  ok = ok && tsBufLiteral(bP, (evP->attrName != NULL) ? evP->attrName : "", NULL);
  ok = ok && tsBufText(bP, ", ");
  ok = ok && tsBufLiteral(bP, (evP->datasetId != NULL && evP->datasetId[0] != 0) ? evP->datasetId : cols.dsId, NULL);
  ok = ok && tsBufText(bP, ", %s, %s, '%s', %d, ", createdExpr, tsExpr, opName(evP->op), cols.kind);
  ok = ok && tsBufLiteral(bP, cols.observedAtIso, "timestamptz");
  ok = ok && tsBufText(bP, ", ");
  ok = ok && tsBufLiteral(bP, cols.v_text, "text");
  ok = ok && tsBufText(bP, ", ");
  ok = ok && tsBufLiteral(bP, cols.v_number, "double precision");
  ok = ok && tsBufText(bP, ", ");
  ok = ok && tsBufLiteral(bP, cols.v_bool, "boolean");
  ok = ok && tsBufText(bP, ", ");
  ok = ok && tsBufLiteral(bP, cols.v_datetime, "timestamptz");
  ok = ok && tsBufText(bP, ", ");
  ok = ok && tsBufLiteral(bP, cols.v_compound, "jsonb");
  ok = ok && tsBufText(bP, ", ");
  ok = ok && tsBufLiteral(bP, cols.sub_attrs, "jsonb");
  ok = ok && tsBufText(bP, ")");

  uP->attrRowN += 1;
  if (ok == false)
    uP->oom = true;
}



// -----------------------------------------------------------------------------
//
// unitExecLocked - send a unit on the thread-local timescaleConn and free it
//
static int unitExecLocked(TsUnit* uP)
{
  int rc = TROE_ERR;

  if (uP == NULL)
    COR_E("timescale: TRoE INSERT: out of memory");
  else if (uP->oom == true)
    COR_E("timescale: TRoE INSERT: out of memory");
  else
    rc = timescaleUnitsExec(timescaleConn, &uP, 1);

  tsUnitFree(uP);
  return rc;
}



// -----------------------------------------------------------------------------
//
// timescaleExecEntityInsertLocked - INSERT one row into troe_entities.
// Operates on the thread-local timescaleConn (set by the entry point).
//
int timescaleExecEntityInsertLocked(const TroeEvent* evP)
{
  TsUnit* uP = tsUnitNew();

  if (uP != NULL)
    entityRowAdd(uP, evP);

  return unitExecLocked(uP);
}



// -----------------------------------------------------------------------------
//
// timescaleExecAttrInsertLocked - INSERT the row(s) of one attribute event into troe_attrs.
// Operates on the thread-local timescaleConn (set by the entry point).
//
int timescaleExecAttrInsertLocked(const TroeEvent* evP)
{
  TsUnit* uP = tsUnitNew();

  if (uP != NULL)
    attrRowAdd(uP, evP);

  return unitExecLocked(uP);
}



// -----------------------------------------------------------------------------
//
// fanOutAttrsFromEntity - on entityCreated, write a per-attr row for each
// top-level attr in entitySnapshot.
//
// The create routines defer one entity-level event (vs N attr events, which
// would force every routine to walk its own write). The timescale plugin owns
// the per-attr expansion at write time so a temporal-query GET sees the
// initial state too. A Replace is not expanded here - it defers its own
// per-Attribute events.
//
static void fanOutAttrsFromEntity(TsUnit* uP, const TroeEvent* evP)
{
  if (evP->entitySnapshot == NULL) return;

  TroeOp attrOp = TroeOpAttrCreated;

  for (CorNode* attrP = evP->entitySnapshot->value.head; attrP != NULL; attrP = attrP->next)
  {
    if (attrP->name == NULL)                       continue;
    if (attrP->name[0] == '@')                     continue;
    if (ldTermId(attrP) == CorTermId)  continue;
    if (strcmp(attrP->name, "_id")        == 0)    continue;
    if (ldTermId(attrP) == CorTermType)        continue;
    if (ldTermId(attrP) == CorTermScope)       continue;
    if (ldTermId(attrP) == CorTermCreatedAt)   continue;
    if (ldTermId(attrP) == CorTermModifiedAt)  continue;

    TroeEvent attrEv;
    memset(&attrEv, 0, sizeof(attrEv));
    attrEv.op             = attrOp;
    attrEv.tenantP        = evP->tenantP;
    attrEv.entityId       = evP->entityId;
    attrEv.entityType     = evP->entityType;
    attrEv.attrName       = attrP->name;
    attrEv.modifiedAtNs   = evP->modifiedAtNs;
    attrEv.attrSnapshot   = attrP;
    attrEv.entitySnapshot = evP->entitySnapshot;

    attrRowAdd(uP, &attrEv);
  }
}



// -----------------------------------------------------------------------------
//
// eventListRecord - the rows of an event list as one unit: to the tenant's writers, or sent here
//
// The event list is built per request (troeDispatchPending drains the per-request queue), so every
// event in one list shares the request's tenant.
//
// wait: the caller is told how it went - the writers' commit is waited for. Without it the unit
// is the writers' once submitted, and this returns at once (the deferred dispatch - after the
// response).
//
static int eventListRecord(const TroeEvent* listHead, bool wait)
{
  if (listHead == NULL) return TROE_ERR;

  TsUnit* uP = tsUnitNew();

  if (uP == NULL)
  {
    COR_E("timescale: TRoE INSERT: out of memory");
    return TROE_ERR;
  }

  for (const TroeEvent* evP = listHead; evP != NULL; evP = evP->next)
  {
    if (evP->op >= TroeOpAttrCreated)
      attrRowAdd(uP, evP);
    else
    {
      entityRowAdd(uP, evP);

      // Entity-level create / replace fan out into per-attr rows so
      // the initial state is queryable on the temporal-attrs side.
      //
      // Created only: a Replace defers an event of its own per Attribute of its
      // body (and a deletion per instance it removed), so fanning out its entity
      // event too wrote every one of its rows TWICE.
      //
      if (evP->op == TroeOpEntityCreated)
        fanOutAttrsFromEntity(uP, evP);
    }
  }

  if (uP->oom == true)
  {
    COR_E("timescale: TRoE INSERT: out of memory");
    tsUnitFree(uP);
    return TROE_ERR;
  }

  int rc = TROE_OK;

  if (timescaleWriterSubmit(listHead->tenantP, uP, wait, &rc) == true)
    return rc;

  // No writers (--troeWriters 0, or shutting down): one transaction of its own, here
  TimescaleConn* cP = timescaleConnGet(listHead->tenantP);

  if (cP == NULL)
  {
    tsUnitFree(uP);
    return TROE_ERR;
  }

  timescaleConn = cP->conn;
  rc            = unitExecLocked(uP);
  timescaleConn = NULL;
  timescaleConnRelease(cP);

  return rc;
}



// -----------------------------------------------------------------------------
//
// timescaleEventList - record a list of events, as one transaction
//
// --troeSync waits for the commit: the response is not to leave before its history is recorded.
//
int timescaleEventList(const TroeEvent* listHead, int count)
{
  (void) count;
  return eventListRecord(listHead, troeSync);
}



// -----------------------------------------------------------------------------
//
// timescaleHistoryImport - the events of an import (coraine-import): as an event list, waited for -
// the import reports what did not make it, and exits when it is done
//
int timescaleHistoryImport(const TroeEvent* listHead, int count)
{
  (void) count;
  return eventListRecord(listHead, true);
}

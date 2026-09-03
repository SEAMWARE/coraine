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
// columns and the sub_attrs JSONB. Each entry point acquires a connection
// from the tenant's pool (thread-local timescaleConn).
//

#include <stddef.h>                                       // NULL
#include <stdio.h>                                        // snprintf
#include <stdlib.h>                                       // free
#include <string.h>                                       // strcmp
#include <time.h>                                         // gmtime_r, time_t
#include <libpq-fe.h>                                     // PG*

#include "ktrace/kTrace.h"                                // KT_E

#include "kjson/KjNode.h"                                 // KjNode
#include "kjson/kjLookup.h"                               // kjLookup
#include "kjson/kjBuilder.h"                              // kjObject, kjChildAdd
#include "kjson/kjClone.h"                                // kjClone
#include "kjson/kjRender.h"                               // kjFastRender
#include "kjson/kjRenderSize.h"                           // kjFastRenderSize
#include "kalloc/kaAlloc.h"                               // kaAlloc
#include "kalloc/kaStrdup.h"                              // kaStrdup

#include "corRest/CorRestState.h"                           // corRest
#include "corNgsild/LdAttrType.h"                          // LdAttr*
#include "corNgsild/ldAttrTypeDetect.h"                    // ldAttrTypeDetect

#include "troe/TroeDriver.h"                              // TroeEvent, TroeOp*

#include "temporal/timescale/timescaleGlobals.h"          // timescaleConn
#include "temporal/timescale/timescalePool.h"             // timescaleConnGet, timescaleConnRelease
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
static const char* numberToText(KjNode* nP, KAlloc* allocP)
{
  char buf[64];
  if (nP->type == KjInt)
    snprintf(buf, sizeof(buf), "%lld", (long long) nP->value.i);
  else
    snprintf(buf, sizeof(buf), "%.17g", nP->value.f);
  return kaStrdup(allocP, buf);
}



// -----------------------------------------------------------------------------
//
// renderJsonb - render a KjNode subtree as JSON text suitable for ::jsonb.
//
static const char* renderJsonb(KjNode* nP, KAlloc* allocP)
{
  int   sz  = kjFastRenderSize(nP) + 1;
  char* buf = (char*) kaAlloc(allocP, sz);
  kjFastRender(nP, buf);
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
static void extractCols(KjNode* attrSnapshot, AttrCols* cP)
{
  memset(cP, 0, sizeof(*cP));
  cP->dsId = "";

  if (attrSnapshot == NULL || attrSnapshot->type != KjObject)
    return;

  // First child = first dataset instance.
  KjNode* instP = attrSnapshot->value.firstChildP;
  if (instP == NULL || instP->type != KjObject)
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
  KjNode* valueP   = NULL;
  KjNode* observP  = NULL;
  KjNode* subAttrs = kjObject(corRest.kjsonP, NULL);

  // Read-only walk: nothing below re-homes a node out of instP (see the
  // sub-attribute branch), so the list stays whole. nextP is still captured up
  // front - it costs nothing and it is what keeps this loop correct if anyone
  // ever adds a step that does touch the list.
  KjNode* fP = instP->value.firstChildP;
  while (fP != NULL)
  {
    KjNode* nextP = fP->next;

    if (fP->name == NULL) { fP = nextP; continue; }

    if (strcmp(fP->name, "type")       == 0) { fP = nextP; continue; }
    if (strcmp(fP->name, "createdAt")  == 0) { fP = nextP; continue; }
    if (strcmp(fP->name, "modifiedAt") == 0) { fP = nextP; continue; }
    if (strcmp(fP->name, "datasetId")  == 0) { fP = nextP; continue; }

    if (strcmp(fP->name, "observedAt") == 0)   { observP = fP;       fP = nextP; continue; }
    if (strcmp(fP->name, "value")      == 0 ||
        strcmp(fP->name, "object")     == 0 ||
        strcmp(fP->name, "languageMap") == 0 ||
        strcmp(fP->name, "vocab")      == 0 ||
        strcmp(fP->name, "valueList")  == 0 ||
        strcmp(fP->name, "objectList") == 0 ||
        strcmp(fP->name, "json")       == 0)   { valueP  = fP;       fP = nextP; continue; }

    //
    // Sub-attribute. It belongs to the CALLER's entity tree, so it is CLONED
    // in - neither moved nor, as it was, spliced.
    //
    // kjChildAdd re-points the added node's ->next, and there being no
    // kjChildMove it left the two lists SHARING their tails: the first
    // sub-attribute's ->next was nulled, then the second sub-attribute relinked
    // it, so {type,value,unitCode,observedAt,accuracy} came back out of here as
    // {type,value,unitCode,accuracy} - whatever sat between two sub-attributes
    // spliced out of the attribute we were handed. observedAt is what sits
    // between them in the most ordinary attribute there is.
    //
    // kjChildRemove-then-kjChildAdd is the correct idiom for a MOVE and does
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
    kjChildAdd(subAttrs, kjClone(corRest.kjsonP, fP));
    fP = nextP;
  }

  // observedAt:
  //   - KjString: ISO text, pass through.
  //   - KjInt:    epoch nanoseconds (ldApiEntityToDbModel normalises ISO → ns).
  //               Format as fractional-seconds for postgres ::timestamptz cast.
  if (observP != NULL)
  {
    if (observP->type == KjString)
    {
      cP->observedAtIso = observP->value.s;
    }
    else if (observP->type == KjInt)
    {
      char* buf = (char*) kaAlloc(&corRest.kalloc, 64);
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
      case KjString:  cP->v_text     = valueP->value.s; break;
      case KjInt:
      case KjFloat:   cP->v_number   = numberToText(valueP, &corRest.kalloc); break;
      case KjBoolean: cP->v_bool     = valueP->value.b ? "t" : "f"; break;
      case KjObject:
      case KjArray:   cP->v_compound = renderJsonb(valueP, &corRest.kalloc); break;
      case KjNull:    /* leave all NULL */ break;
      default: break;
    }
  }

  // sub_attrs only emitted if non-empty.
  if (subAttrs->value.firstChildP != NULL)
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
  KjNode* typeP = (evP->entitySnapshot != NULL) ? kjLookup(evP->entitySnapshot, "type") : NULL;
  int     pos   = 0;

  buf[pos++] = '{';

  if ((typeP != NULL) && (typeP->type == KjArray))
  {
    bool first = true;

    for (KjNode* tP = typeP->value.firstChildP; tP != NULL; tP = tP->next)
    {
      if ((tP->type != KjString) || (tP->value.s == NULL))
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

    if ((typeP != NULL) && (typeP->type == KjString))
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
// timescaleExecEntityInsertLocked - INSERT one row into troe_entities.
// Operates on the thread-local timescaleConn (set by the entry point).
//
int timescaleExecEntityInsertLocked(const TroeEvent* evP)
{
  char tsExpr[64];
  timescaleNsToSqlTimestamp(evP->modifiedAtNs, tsExpr, sizeof(tsExpr));

  char typeLit[1024];

  const char* paramV[3];
  paramV[0] = evP->entityId != NULL ? evP->entityId : "";
  paramV[1] = entityTypeArrayLiteral(evP, typeLit, sizeof(typeLit));
  paramV[2] = opName(evP->op);

  char sql[512];
  snprintf(sql, sizeof(sql),
           "INSERT INTO troe_entities (entity_id, entity_type, modified_at, op) "
           "VALUES ($1, $2::text[], %s, $3) "
           "ON CONFLICT (entity_id, modified_at, op) DO NOTHING",
           tsExpr);

  PGresult* res = PQexecParams(timescaleConn, sql, 3, NULL, paramV, NULL, NULL, 0);
  ExecStatusType st = PQresultStatus(res);
  if (st != PGRES_COMMAND_OK)
  {
    KT_E("timescale: troe_entities INSERT failed: %s", PQerrorMessage(timescaleConn));
    PQclear(res);
    return TROE_ERR;
  }
  PQclear(res);
  return TROE_OK;
}



// -----------------------------------------------------------------------------
//
// timescaleExecAttrInsertLocked - INSERT one row into troe_attrs.
// Operates on the thread-local timescaleConn (set by the entry point).
//
int timescaleExecAttrInsertLocked(const TroeEvent* evP)
{
  AttrCols cols;

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
    KjNode* wrapP = (KjNode*) evP->attrSnapshot;
    KjNode* instP = (wrapP != NULL && wrapP->type == KjObject) ? wrapP->value.firstChildP : NULL;
    if (instP != NULL && instP->type == KjObject)
      cols.kind = (int) ldAttrTypeDetect(instP);
  }
  else
    extractCols((KjNode*) evP->attrSnapshot, &cols);

  cols.dsId = (cols.dsId != NULL) ? cols.dsId : "";

  char tsExpr[64];
  timescaleNsToSqlTimestamp(evP->modifiedAtNs, tsExpr, sizeof(tsExpr));

  // Bind params:
  //   $1 entity_id, $2 entity_type, $3 attr_name, $4 dataset_id, $5 op,
  //   $6 attr_kind, $7 observed_at iso, $8 v_text, $9 v_number, $10 v_bool,
  //   $11 v_datetime, $12 v_compound, $13 sub_attrs
  const char* paramV[13];
  char        kindBuf[16];
  char        typeLit[1024];
  snprintf(kindBuf, sizeof(kindBuf), "%d", cols.kind);

  paramV[0]  = evP->entityId != NULL ? evP->entityId : "";
  paramV[1]  = entityTypeArrayLiteral(evP, typeLit, sizeof(typeLit));
  paramV[2]  = evP->attrName   != NULL ? evP->attrName   : "";
  paramV[3]  = (evP->datasetId != NULL && evP->datasetId[0] != 0) ? evP->datasetId : cols.dsId;
  paramV[4]  = opName(evP->op);
  paramV[5]  = kindBuf;
  paramV[6]  = cols.observedAtIso;
  paramV[7]  = cols.v_text;
  paramV[8]  = cols.v_number;
  paramV[9]  = cols.v_bool;
  paramV[10] = cols.v_datetime;
  paramV[11] = cols.v_compound;
  paramV[12] = cols.sub_attrs;

  // No ON CONFLICT — instance_id (PK) is auto-generated by the
  // table's DEFAULT, so each row is unique by construction.
  // created_at and modified_at are seeded to the same instant on insert;
  // Modify-Instance later bumps modified_at while leaving created_at fixed.
  char sql[2048];
  snprintf(sql, sizeof(sql),
           "INSERT INTO troe_attrs ("
           "  entity_id, entity_type, attr_name, dataset_id, created_at, modified_at, op, attr_kind, "
           "  observed_at, v_text, v_number, v_bool, v_datetime, v_compound, sub_attrs) "
           // An attribute event carries no entity snapshot, so $2 can come out
           // empty for a multi-type Entity; fall back to the type list already
           // recorded for this Entity rather than storing nothing.
           "VALUES ($1, COALESCE(NULLIF($2::text[], '{}'),"
           "                     (SELECT entity_type FROM troe_entities"
           "                       WHERE entity_id = $1"
           "                       ORDER BY modified_at DESC LIMIT 1),"
           "                     '{}'::text[]), $3, $4, %s, %s, $5, $6::smallint, "
           "        $7::timestamptz, $8, $9::double precision, $10::boolean, $11::timestamptz, "
           "        $12::jsonb, $13::jsonb)",
           tsExpr, tsExpr);

  PGresult* res = PQexecParams(timescaleConn, sql, 13, NULL, paramV, NULL, NULL, 0);
  ExecStatusType st = PQresultStatus(res);
  if (st != PGRES_COMMAND_OK)
  {
    KT_E("timescale: troe_attrs INSERT failed: %s", PQerrorMessage(timescaleConn));
    PQclear(res);
    return TROE_ERR;
  }
  PQclear(res);
  return TROE_OK;
}



// -----------------------------------------------------------------------------
//
// fanOutAttrsFromEntity - on entityCreated / entityReplaced, write a
// per-attr row for each top-level attr in entitySnapshot.
//
// Service routines defer one entity-level event for create/replace
// (vs N attr events, which would force every routine to walk its own
// write). The timescale plugin owns the per-attr expansion at write
// time so a temporal-query GET sees the initial state too.
//
static int fanOutAttrsFromEntity(const TroeEvent* evP)
{
  if (evP->entitySnapshot == NULL) return TROE_OK;

  TroeOp attrOp = (evP->op == TroeOpEntityReplaced)
                  ? TroeOpAttrReplaced
                  : TroeOpAttrCreated;

  for (KjNode* attrP = evP->entitySnapshot->value.firstChildP; attrP != NULL; attrP = attrP->next)
  {
    if (attrP->name == NULL)                       continue;
    if (attrP->name[0] == '@')                     continue;
    if (strcmp(attrP->name, "id")         == 0)    continue;
    if (strcmp(attrP->name, "_id")        == 0)    continue;
    if (strcmp(attrP->name, "type")       == 0)    continue;
    if (strcmp(attrP->name, "scope")      == 0)    continue;
    if (strcmp(attrP->name, "createdAt")  == 0)    continue;
    if (strcmp(attrP->name, "modifiedAt") == 0)    continue;

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

    int r = timescaleExecAttrInsertLocked(&attrEv);
    if (r != TROE_OK) return r;
  }
  return TROE_OK;
}



// -----------------------------------------------------------------------------
//
// timescaleEventList - drain a queue of events as one transaction.
//
// The deferred-event queue is built per request (troeDispatchPending drains
// the per-request queue), so every event in one list shares the request's
// tenant — a single connection / single transaction covers the whole list.
int timescaleEventList(const TroeEvent* listHead, int count)
{
  if (listHead == NULL) return TROE_ERR;
  (void) count;

  TimescaleConn* cP = timescaleConnGet(listHead->tenantP);
  if (cP == NULL) return TROE_ERR;
  timescaleConn = cP->conn;

  PGresult* beginR = PQexec(timescaleConn, "BEGIN");
  if (PQresultStatus(beginR) != PGRES_COMMAND_OK)
  {
    KT_E("timescale: BEGIN failed: %s", PQerrorMessage(timescaleConn));
    PQclear(beginR);
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return TROE_ERR;
  }
  PQclear(beginR);

  int rc = TROE_OK;
  for (const TroeEvent* evP = listHead; evP != NULL; evP = evP->next)
  {
    int r;
    if (evP->op >= TroeOpAttrCreated)
    {
      r = timescaleExecAttrInsertLocked(evP);
    }
    else
    {
      r = timescaleExecEntityInsertLocked(evP);

      // Entity-level create / replace fan out into per-attr rows so
      // the initial state is queryable on the temporal-attrs side.
      if (r == TROE_OK && (evP->op == TroeOpEntityCreated || evP->op == TroeOpEntityReplaced))
        r = fanOutAttrsFromEntity(evP);
    }

    if (r != TROE_OK) { rc = r; break; }
  }

  if (rc == TROE_OK)
  {
    PGresult* commitR = PQexec(timescaleConn, "COMMIT");
    if (PQresultStatus(commitR) != PGRES_COMMAND_OK)
    {
      KT_E("timescale: COMMIT failed: %s", PQerrorMessage(timescaleConn));
      rc = TROE_ERR;
    }
    PQclear(commitR);
  }
  else
  {
    PGresult* rollbackR = PQexec(timescaleConn, "ROLLBACK");
    PQclear(rollbackR);
  }

  timescaleConn = NULL;
  timescaleConnRelease(cP);
  return rc;
}

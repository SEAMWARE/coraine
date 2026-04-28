//
// FILE            timescaleEvent.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Write path for the timescale TRoE plugin. Walks the attrSnapshot to
// extract typed values into the v_text / v_number / v_bool / v_compound
// columns and the sub_attrs JSONB. Single connection + mutex.
//

#include <stddef.h>                                       // NULL
#include <stdio.h>                                        // snprintf
#include <stdlib.h>                                       // free
#include <string.h>                                       // strcmp
#include <time.h>                                         // gmtime_r, time_t
#include <pthread.h>                                      // pthread_mutex_lock
#include <libpq-fe.h>                                     // PG*

#include "ktrace/kTrace.h"                                // KT_E

#include "kjson/KjNode.h"                                 // KjNode
#include "kjson/kjLookup.h"                               // kjLookup
#include "kjson/kjBuilder.h"                              // kjObject, kjChildAdd
#include "kjson/kjRender.h"                               // kjFastRender
#include "kjson/kjRenderSize.h"                           // kjFastRenderSize
#include "kalloc/kaAlloc.h"                               // kaAlloc
#include "kalloc/kaStrdup.h"                              // kaStrdup

#include "swRest/SwRestState.h"                           // swRest
#include "swNgsild/LdAttrType.h"                          // LdAttr*
#include "swNgsild/ldAttrTypeDetect.h"                    // ldAttrTypeDetect

#include "troe/TroeDriver.h"                              // TroeEvent, TroeOp*

#include "temporal/timescale/timescaleGlobals.h"          // timescaleConn, timescaleMutex
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
// nsToSqlTimestamp - epoch nanoseconds → "to_timestamp(<seconds>)" SQL fragment.
//
static void nsToSqlTimestamp(uint64_t ns, char* buf, int bufSize)
{
  double secs = (double) ns / 1e9;
  snprintf(buf, bufSize, "to_timestamp(%.6f)", secs);
}



// -----------------------------------------------------------------------------
//
// AttrCols - column values extracted from one attribute snapshot.
//
// All `const char*` pointers — NULL means "DON'T BIND" (column stays
// NULL in SQL via DEFAULT). Strings live in swRest.kalloc, no free.
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

  // ldApiEntityToDbModel normalises all value-bearing keys (value/object/
  // languageMap/vocab/valueList/objectList/json) to "value" in storage.
  // Attr-kind already disambiguates; we only need to read the "value" field.
  KjNode* valueP   = NULL;
  KjNode* observP  = NULL;
  KjNode* subAttrs = kjObject(swRest.kjsonP, NULL);

  for (KjNode* fP = instP->value.firstChildP; fP != NULL; fP = fP->next)
  {
    if (fP->name == NULL) continue;

    if (strcmp(fP->name, "type")       == 0) continue;
    if (strcmp(fP->name, "value")      == 0) { valueP  = fP; continue; }
    if (strcmp(fP->name, "observedAt") == 0) { observP = fP; continue; }
    if (strcmp(fP->name, "createdAt")  == 0) continue;
    if (strcmp(fP->name, "modifiedAt") == 0) continue;
    if (strcmp(fP->name, "datasetId")  == 0) continue;

    // Sub-attribute. Add a copy by name. (Plugin doesn't own the lifetime
    // of the rendered JSON beyond this row write, so a shallow link is fine.)
    KjNode* clone = fP;
    kjChildAdd(subAttrs, clone);
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
      char* buf = (char*) kaAlloc(&swRest.kalloc, 64);
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
      case KjFloat:   cP->v_number   = numberToText(valueP, &swRest.kalloc); break;
      case KjBoolean: cP->v_bool     = valueP->value.b ? "t" : "f"; break;
      case KjObject:
      case KjArray:   cP->v_compound = renderJsonb(valueP, &swRest.kalloc); break;
      case KjNull:    /* leave all NULL */ break;
      default: break;
    }
  }

  // sub_attrs only emitted if non-empty.
  if (subAttrs->value.firstChildP != NULL)
    cP->sub_attrs = renderJsonb(subAttrs, &swRest.kalloc);
}



// -----------------------------------------------------------------------------
//
// timescaleExecEntityInsertLocked - INSERT one row into troe_entities.
// Caller must hold timescaleMutex.
//
int timescaleExecEntityInsertLocked(const TroeEvent* evP)
{
  char tsExpr[64];
  nsToSqlTimestamp(evP->modifiedAtNs, tsExpr, sizeof(tsExpr));

  const char* paramV[3];
  paramV[0] = evP->entityId   != NULL ? evP->entityId   : "";
  paramV[1] = evP->entityType != NULL ? evP->entityType : "";
  paramV[2] = opName(evP->op);

  char sql[512];
  snprintf(sql, sizeof(sql),
           "INSERT INTO troe_entities (entity_id, entity_type, modified_at, op) "
           "VALUES ($1, $2, %s, $3) "
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
// Caller must hold timescaleMutex.
//
int timescaleExecAttrInsertLocked(const TroeEvent* evP)
{
  AttrCols cols;
  extractCols((KjNode*) evP->attrSnapshot, &cols);

  // For deletion events, attrSnapshot is the post-delete entity (attr is
  // gone). Don't try to bind value cols.
  if (evP->op == TroeOpAttrDeleted)
    memset(&cols, 0, sizeof(cols));
  cols.dsId = (cols.dsId != NULL) ? cols.dsId : "";

  char tsExpr[64];
  nsToSqlTimestamp(evP->modifiedAtNs, tsExpr, sizeof(tsExpr));

  // Bind params:
  //   $1 entity_id, $2 entity_type, $3 attr_name, $4 dataset_id,
  //   $5 op, $6 attr_kind (text → SQL casts to smallint),
  //   $7 observed_at iso (or NULL), $8 v_text, $9 v_number,
  //   $10 v_bool, $11 v_datetime, $12 v_compound, $13 sub_attrs
  const char* paramV[13];
  char        kindBuf[16];
  snprintf(kindBuf, sizeof(kindBuf), "%d", cols.kind);

  paramV[0]  = evP->entityId   != NULL ? evP->entityId   : "";
  paramV[1]  = evP->entityType != NULL ? evP->entityType : "";
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

  char sql[1024];
  snprintf(sql, sizeof(sql),
           "INSERT INTO troe_attrs ("
           "  entity_id, entity_type, attr_name, dataset_id, modified_at, op, attr_kind, "
           "  observed_at, v_text, v_number, v_bool, v_datetime, v_compound, sub_attrs) "
           "VALUES ($1, $2, $3, $4, %s, $5, $6::smallint, "
           "        $7::timestamptz, $8, $9::double precision, $10::boolean, $11::timestamptz, "
           "        $12::jsonb, $13::jsonb) "
           "ON CONFLICT (entity_id, attr_name, dataset_id, modified_at, op) DO NOTHING",
           tsExpr);

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
// timescaleEntityEvent -
//
int timescaleEntityEvent(const TroeEvent* evP)
{
  if (evP == NULL || timescaleConn == NULL) return TROE_ERR;

  pthread_mutex_lock(&timescaleMutex);
  int r = timescaleExecEntityInsertLocked(evP);

  // For entity-level create / replace, fan out into per-attr rows so
  // the initial state is queryable on the temporal-attrs side.
  if (r == TROE_OK && (evP->op == TroeOpEntityCreated || evP->op == TroeOpEntityReplaced))
    r = fanOutAttrsFromEntity(evP);

  pthread_mutex_unlock(&timescaleMutex);
  return r;
}



// -----------------------------------------------------------------------------
//
// timescaleAttrEvent -
//
int timescaleAttrEvent(const TroeEvent* evP)
{
  if (evP == NULL || timescaleConn == NULL) return TROE_ERR;

  pthread_mutex_lock(&timescaleMutex);
  int r = timescaleExecAttrInsertLocked(evP);
  pthread_mutex_unlock(&timescaleMutex);
  return r;
}



// -----------------------------------------------------------------------------
//
// timescaleEventList - drain a queue of events as one transaction.
//
int timescaleEventList(const TroeEvent* listHead, int count)
{
  if (listHead == NULL || timescaleConn == NULL) return TROE_ERR;
  (void) count;

  pthread_mutex_lock(&timescaleMutex);

  PGresult* beginR = PQexec(timescaleConn, "BEGIN");
  if (PQresultStatus(beginR) != PGRES_COMMAND_OK)
  {
    KT_E("timescale: BEGIN failed: %s", PQerrorMessage(timescaleConn));
    PQclear(beginR);
    pthread_mutex_unlock(&timescaleMutex);
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

  pthread_mutex_unlock(&timescaleMutex);
  return rc;
}

//
// FILE            timescaleHistoryWrite.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Direct write paths for the temporal-write endpoints (§ 5.6.11,
// § 5.6.12, § 5.6.13, § 5.6.16). Bypass the current-state DB and
// the event-deferral pipeline — the client is dictating history
// directly, so we INSERT (or DELETE) into the TRoE tables under
// one transaction on a connection from the tenant's pool.
//
// The Create / Add-Attrs paths reuse the row-insert primitives in
// timescaleEvent.c by wrapping each EntityTemporal instance in a
// `{"<datasetId>": <instance>}` envelope and synthesising a TroeEvent.
// Per-instance modifiedAtNs values are offset by 1 ms each so the
// schema's (entity_id, attr_name, dataset_id, modified_at, op) PK
// stays unique even when many instances share an observedAt.
//

#include <stddef.h>                                       // NULL
#include <stdbool.h>                                      // bool
#include <stdint.h>                                       // uint64_t
#include <stdio.h>                                        // snprintf
#include <stdlib.h>                                       // strtoll
#include <string.h>                                       // strcmp, memset
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

#include "troe/TroeDriver.h"                              // TroeEvent, TROE_*

#include "temporal/timescale/timescaleGlobals.h"          // timescaleConn
#include "temporal/timescale/timescalePool.h"             // timescaleConnGet, timescaleConnRelease, timescalePoolDrop
#include "temporal/timescale/timescaleEvent.h"            // timescaleExec*Locked
#include "temporal/timescale/timescaleHistoryWrite.h"     // Own interface



// -----------------------------------------------------------------------------
//
// entityHasRows - quick existence check against troe_entities.
// Operates on the thread-local timescaleConn.
//
static bool entityHasRows(const char* tenant, const char* entityId)
{
  const char* paramV[2] = { entityId, tenant };
  PGresult* res = PQexecParams(timescaleConn,
    "SELECT 1 FROM troe_entities WHERE entity_id = $1 AND tenant = $2 LIMIT 1",
    2, NULL, paramV, NULL, NULL, 0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    KT_E("timescale: troe_entities existence SELECT failed: %s",
         PQerrorMessage(timescaleConn));
    PQclear(res);
    return false;
  }
  bool exists = (PQntuples(res) > 0);
  PQclear(res);
  return exists;
}



// -----------------------------------------------------------------------------
//
// attrHasRows - quick existence check against troe_attrs for a given attr.
// Operates on the thread-local timescaleConn.
//
static bool attrHasRows(const char* tenant, const char* entityId, const char* attrName)
{
  const char* paramV[3] = { entityId, attrName, tenant };
  PGresult* res = PQexecParams(timescaleConn,
    "SELECT 1 FROM troe_attrs WHERE entity_id = $1 AND attr_name = $2 AND tenant = $3 LIMIT 1",
    3, NULL, paramV, NULL, NULL, 0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    KT_E("timescale: troe_attrs existence SELECT failed: %s",
         PQerrorMessage(timescaleConn));
    PQclear(res);
    return false;
  }
  bool exists = (PQntuples(res) > 0);
  PQclear(res);
  return exists;
}



// -----------------------------------------------------------------------------
//
// timescaleTenantDrop - drop a tenant's entire temporal database.
//
// Used by the snapshot lifecycle: when a snapshot is deleted or purged,
// the broker calls db.tenantDrop on the snap-tenant; this is the TRoE
// counterpart so the per-snapshot temporal database doesn't leak.
//
// Each tenant owns its own physical database, so the drop is a single
// DROP DATABASE — instant and bloat-free, vs the old DELETE-WHERE-tenant.
// The default tenant is never dropped (timescalePoolDrop guards on the
// empty name).
//
int timescaleTenantDrop(Tenant* tenantP)
{
  return timescalePoolDrop(tenantP);
}



// -----------------------------------------------------------------------------
//
// timescaleEntityTemporalDelete - § 5.6.16 Delete Temporal Evolution.
//
int timescaleEntityTemporalDelete(Tenant* tenantP, const char* entityId)
{
  if (entityId == NULL || entityId[0] == 0)
    return TROE_ERR;

  const char* tenant = (tenantP != NULL) ? tenantP->name : "";

  TimescaleConn* cP = timescaleConnGet(tenantP);
  if (cP == NULL) return TROE_ERR;
  timescaleConn = cP->conn;

  if (!entityHasRows(tenant, entityId))
  {
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return TROE_NOT_FOUND;
  }

  const char* paramV[2] = { entityId, tenant };

  // Single statement per table — both DELETEs run in autocommit since
  // they're independent and either succeeding leaves the store in a
  // consistent (partial) state. For atomicity we wrap in BEGIN/COMMIT.
  PGresult* r = PQexec(timescaleConn, "BEGIN");
  PQclear(r);

  PGresult* attrsR = PQexecParams(timescaleConn,
    "DELETE FROM troe_attrs WHERE entity_id = $1 AND tenant = $2",
    2, NULL, paramV, NULL, NULL, 0);
  if (PQresultStatus(attrsR) != PGRES_COMMAND_OK)
  {
    KT_E("timescale: troe_attrs DELETE failed: %s", PQerrorMessage(timescaleConn));
    PQclear(attrsR);
    PQclear(PQexec(timescaleConn, "ROLLBACK"));
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return TROE_ERR;
  }
  PQclear(attrsR);

  PGresult* entR = PQexecParams(timescaleConn,
    "DELETE FROM troe_entities WHERE entity_id = $1 AND tenant = $2",
    2, NULL, paramV, NULL, NULL, 0);
  if (PQresultStatus(entR) != PGRES_COMMAND_OK)
  {
    KT_E("timescale: troe_entities DELETE failed: %s", PQerrorMessage(timescaleConn));
    PQclear(entR);
    PQclear(PQexec(timescaleConn, "ROLLBACK"));
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return TROE_ERR;
  }
  PQclear(entR);

  PQclear(PQexec(timescaleConn, "COMMIT"));
  timescaleConn = NULL;
  timescaleConnRelease(cP);
  return TROE_OK;
}



// -----------------------------------------------------------------------------
//
// timescaleEntityTemporalAttrDelete - § 5.6.13 Delete Attribute from Temporal.
//
// deleteAll==true → all instances of the attr regardless of datasetId.
// deleteAll==false + datasetId==NULL → only the default dataset (dataset_id='').
// deleteAll==false + datasetId!=NULL → only that dataset.
//
int timescaleEntityTemporalAttrDelete(Tenant* tenantP, const char* entityId,
                                      const char* attrName,
                                      const char* datasetId, bool deleteAll)
{
  if (entityId == NULL || attrName == NULL)
    return TROE_ERR;

  const char* tenant = (tenantP != NULL) ? tenantP->name : "";

  TimescaleConn* cP = timescaleConnGet(tenantP);
  if (cP == NULL) return TROE_ERR;
  timescaleConn = cP->conn;

  if (!entityHasRows(tenant, entityId))
  {
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return TROE_NOT_FOUND;
  }
  if (!attrHasRows(tenant, entityId, attrName))
  {
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return TROE_NOT_FOUND;
  }

  PGresult* res = NULL;
  if (deleteAll)
  {
    const char* paramV[3] = { entityId, attrName, tenant };
    res = PQexecParams(timescaleConn,
      "DELETE FROM troe_attrs WHERE entity_id = $1 AND attr_name = $2 AND tenant = $3",
      3, NULL, paramV, NULL, NULL, 0);
  }
  else
  {
    const char* paramV[4] = { entityId, attrName, (datasetId != NULL) ? datasetId : "", tenant };
    res = PQexecParams(timescaleConn,
      "DELETE FROM troe_attrs WHERE entity_id = $1 AND attr_name = $2 AND dataset_id = $3 AND tenant = $4",
      4, NULL, paramV, NULL, NULL, 0);
  }

  if (PQresultStatus(res) != PGRES_COMMAND_OK)
  {
    KT_E("timescale: troe_attrs attr-delete failed: %s", PQerrorMessage(timescaleConn));
    PQclear(res);
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return TROE_ERR;
  }
  PQclear(res);

  timescaleConn = NULL;
  timescaleConnRelease(cP);
  return TROE_OK;
}



// -----------------------------------------------------------------------------
//
// instanceWrap - wrap an EntityTemporal instance into the DB-model snapshot
// shape that timescaleExecAttrInsertLocked / extractCols expects:
//   { "<datasetId-or-@none>": <instance> }
//
// The wrapper is a fresh KjObject; the instance child node is rehomed (its
// `next` pointer is stomped) — caller must not iterate the original parent
// array further after wrapping.
//
static KjNode* instanceWrap(KjNode* instanceP)
{
  KjNode* dsP = kjLookup(instanceP, "datasetId");
  const char* dsKey = (dsP != NULL && dsP->type == KjString) ? dsP->value.s : "@none";

  KjNode* wrap = kjObject(swRest.kjsonP, NULL);
  // Detach the instance from its array parent and rename for the wrapper.
  instanceP->next = NULL;
  instanceP->name = (char*) dsKey;
  kjChildAdd(wrap, instanceP);
  return wrap;
}



// -----------------------------------------------------------------------------
//
// insertInstanceRows - walk an EntityTemporal tree and insert one row per
// (attribute, instance) into troe_attrs. Operates on the thread-local
// timescaleConn, within a transaction begun by the caller.
//
// modAtNs0 = base modified-at (request start time). Successive instances
// receive +1 ms offsets so the PK stays unique under shared observedAt.
//
static int insertInstanceRows(Tenant* tenantP, const char* entityId,
                              const char* entityType, KjNode* rootP,
                              uint64_t modAtNs0)
{
  uint64_t nsOffset = 0;

  // IMPORTANT: instanceWrap stomps the wrapped node's ->next, which
  // breaks this iteration if attrP itself is wrapped (single-object
  // attr branch). Capture nextP up-front and we're safe in both branches.
  KjNode* attrP = rootP->value.firstChildP;
  while (attrP != NULL)
  {
    KjNode* nextAttrP = attrP->next;

    if (attrP->name == NULL ||
        attrP->name[0] == '@' ||
        strcmp(attrP->name, "id")         == 0 ||
        strcmp(attrP->name, "type")       == 0 ||
        strcmp(attrP->name, "scope")      == 0 ||
        strcmp(attrP->name, "createdAt")  == 0 ||
        strcmp(attrP->name, "modifiedAt") == 0)
    {
      attrP = nextAttrP;
      continue;
    }

    // EntityTemporal: each attribute is an array of instance objects.
    // Tolerate single-object form (some clients omit the array).
    if (attrP->type == KjArray)
    {
      // instanceWrap stomps each instance's ->next. Capture nextP before wrapping.
      KjNode* instP = attrP->value.firstChildP;
      while (instP != NULL)
      {
        KjNode* nextInstP = instP->next;

        TroeEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.op           = TroeOpAttrCreated;
        ev.tenantP      = tenantP;
        ev.entityId     = entityId;
        ev.entityType   = entityType;
        ev.attrName     = attrP->name;
        ev.modifiedAtNs = modAtNs0 + nsOffset;
        ev.attrSnapshot = instanceWrap(instP);

        int r = timescaleExecAttrInsertLocked(&ev);
        if (r != TROE_OK) return r;

        nsOffset += 1000000;  // +1 ms
        instP = nextInstP;
      }
    }
    else if (attrP->type == KjObject)
    {
      TroeEvent ev;
      memset(&ev, 0, sizeof(ev));
      ev.op           = TroeOpAttrCreated;
      ev.tenantP      = tenantP;
      ev.entityId     = entityId;
      ev.entityType   = entityType;
      ev.attrName     = attrP->name;
      ev.modifiedAtNs = modAtNs0 + nsOffset;
      ev.attrSnapshot = instanceWrap(attrP);
      nsOffset += 1000000;

      int r = timescaleExecAttrInsertLocked(&ev);
      if (r != TROE_OK) return r;
    }

    attrP = nextAttrP;
  }
  return TROE_OK;
}



// -----------------------------------------------------------------------------
//
// timescaleEntityTemporalCreate - § 5.6.11 / § 6.18.3.1 — Create or
// Update Temporal Evolution of an Entity.
//
// Spec lets the client send an EntityTemporal Fragment representing the
// initial history. We:
//   1. Insert one troe_entities row (op='created')
//   2. For each attr instance in the body, insert one troe_attrs row.
//
// Caller already validated the body shape and resolved id/type from it.
//
int timescaleEntityTemporalCreate(Tenant* tenantP, KjNode* rootP)
{
  if (rootP == NULL || rootP->type != KjObject)
    return TROE_ERR;

  KjNode* idP   = kjLookup(rootP, "id");
  KjNode* typeP = kjLookup(rootP, "type");
  if (idP == NULL || idP->type != KjString || idP->value.s[0] == 0)
    return TROE_ERR;
  if (typeP == NULL || typeP->type != KjString || typeP->value.s[0] == 0)
    return TROE_ERR;

  const char* entityId   = idP->value.s;
  const char* entityType = typeP->value.s;
  const char* tenant     = (tenantP != NULL) ? tenantP->name : "";

  TimescaleConn* cP = timescaleConnGet(tenantP);
  if (cP == NULL) return TROE_ERR;
  timescaleConn = cP->conn;

  // § 5.6.11.4: if the temporal evolution already exists, the operation
  // is an update — instances are appended. Pre-check so the service
  // routine can return 204 (update) vs 201 (create) per § 6.18.3.1.
  bool existed = false;
  {
    const char* idParam[2] = { entityId, tenant };
    PGresult* eRes = PQexecParams(timescaleConn,
      "SELECT 1 FROM troe_entities WHERE entity_id = $1 AND tenant = $2 LIMIT 1",
      2, NULL, idParam, NULL, NULL, 0);
    if (PQresultStatus(eRes) == PGRES_TUPLES_OK && PQntuples(eRes) > 0)
      existed = true;
    PQclear(eRes);
  }

  PGresult* beginR = PQexec(timescaleConn, "BEGIN");
  PQclear(beginR);

  // Entity row — only on first creation. For updates the entity row
  // already exists; we skip the troe_entities insert so we don't pile
  // up redundant 'created' rows on every append.
  if (!existed)
  {
    TroeEvent eEv;
    memset(&eEv, 0, sizeof(eEv));
    eEv.op           = TroeOpEntityCreated;
    eEv.tenantP      = tenantP;
    eEv.entityId     = entityId;
    eEv.entityType   = entityType;
    eEv.modifiedAtNs = swRest.requestStartTime;

    int r = timescaleExecEntityInsertLocked(&eEv);
    if (r != TROE_OK)
    {
      PQclear(PQexec(timescaleConn, "ROLLBACK"));
      timescaleConn = NULL;
      timescaleConnRelease(cP);
      return r;
    }
  }

  // Attr rows.
  int r = insertInstanceRows(tenantP, entityId, entityType, rootP, swRest.requestStartTime);
  if (r != TROE_OK)
  {
    PQclear(PQexec(timescaleConn, "ROLLBACK"));
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return r;
  }

  PQclear(PQexec(timescaleConn, "COMMIT"));
  timescaleConn = NULL;
  timescaleConnRelease(cP);
  return existed ? TROE_UPDATED : TROE_OK;
}



// -----------------------------------------------------------------------------
//
// extractInstanceFromBody - find the single instance object in the body of a
// PATCH /attrs/{attr}/{instance} request (§ 5.6.14.3).
//
// Tolerate either:
//   (a) { "speed": [ { "type":"Property","value":42, ... } ] }   — fragment
//   (b) { "type":"Property","value":42, "observedAt":"..." }     — bare instance
//
static KjNode* extractInstanceFromBody(KjNode* bodyP)
{
  if (bodyP == NULL || bodyP->type != KjObject)
    return NULL;

  // Case (b): the body itself is the instance.
  KjNode* tP = kjLookup(bodyP, "type");
  if (tP != NULL && tP->type == KjString)
  {
    const char* t = tP->value.s;
    if (strcmp(t, "Property") == 0 || strcmp(t, "Relationship") == 0 ||
        strcmp(t, "GeoProperty") == 0 || strcmp(t, "LanguageProperty") == 0 ||
        strcmp(t, "VocabProperty") == 0 || strcmp(t, "ListProperty") == 0 ||
        strcmp(t, "ListRelationship") == 0 || strcmp(t, "JsonProperty") == 0)
      return bodyP;
  }

  // Case (a): walk children for the first attr-shaped array.
  for (KjNode* fP = bodyP->value.firstChildP; fP != NULL; fP = fP->next)
  {
    if (fP->name == NULL)                           continue;
    if (fP->name[0] == '@')                         continue;
    if (strcmp(fP->name, "id")        == 0)         continue;
    if (strcmp(fP->name, "type")      == 0)         continue;
    if (strcmp(fP->name, "scope")     == 0)         continue;
    if (strcmp(fP->name, "createdAt")  == 0)        continue;
    if (strcmp(fP->name, "modifiedAt") == 0)        continue;

    if (fP->type == KjArray && fP->value.firstChildP != NULL)
      return fP->value.firstChildP;
    if (fP->type == KjObject)
      return fP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// instanceExists - does (entity, attr, instance) name a row? Caller holds mutex.
//
static bool instanceExists(const char* tenant, const char* entityId,
                           const char* attrName, const char* instanceId)
{
  const char* paramV[4] = { entityId, attrName, instanceId, tenant };
  PGresult* res = PQexecParams(timescaleConn,
    "SELECT 1 FROM troe_attrs "
    " WHERE entity_id = $1 AND attr_name = $2 AND instance_id = $3 AND tenant = $4 LIMIT 1",
    4, NULL, paramV, NULL, NULL, 0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    KT_E("timescale: instance lookup failed: %s", PQerrorMessage(timescaleConn));
    PQclear(res);
    return false;
  }
  bool exists = (PQntuples(res) > 0);
  PQclear(res);
  return exists;
}



// -----------------------------------------------------------------------------
//
// timescaleEntityTemporalInstanceModify - § 5.6.14 / § 6.22.3.1.
//
// Replaces the value-bearing columns of the row identified by instanceId
// and bumps modified_at to now() per § 5.6.14.4 ("modifiedAt shall be set
// to the timestamp corresponding to this modification"). The PK is
// instance_id, so modified_at is free to change.
//
int timescaleEntityTemporalInstanceModify(Tenant* tenantP, const char* entityId,
                                          const char* attrName,
                                          const char* instanceId,
                                          KjNode* rootP)
{
  if (entityId == NULL || attrName == NULL ||
      instanceId == NULL || rootP == NULL)
    return TROE_ERR;

  const char* tenant = (tenantP != NULL) ? tenantP->name : "";

  KjNode* instP = extractInstanceFromBody(rootP);
  if (instP == NULL)
    return TROE_ERR;  // Caller maps to 400.

  KjNode* valueP  = kjLookup(instP, "value");
  KjNode* observP = kjLookup(instP, "observedAt");

  // Render typed columns from the instance's value node.
  const char* v_text   = NULL;
  const char* v_number = NULL;
  const char* v_bool   = NULL;
  const char* v_compnd = NULL;

  if (valueP != NULL)
  {
    if (valueP->type == KjString)
      v_text = valueP->value.s;
    else if (valueP->type == KjInt)
    {
      char* buf = (char*) kaAlloc(&swRest.kalloc, 32);
      snprintf(buf, 32, "%lld", (long long) valueP->value.i);
      v_number = buf;
    }
    else if (valueP->type == KjFloat)
    {
      char* buf = (char*) kaAlloc(&swRest.kalloc, 64);
      snprintf(buf, 64, "%.17g", valueP->value.f);
      v_number = buf;
    }
    else if (valueP->type == KjBoolean)
    {
      v_bool = valueP->value.b ? "t" : "f";
    }
    else if (valueP->type == KjObject || valueP->type == KjArray)
    {
      int   sz  = kjFastRenderSize(valueP) + 1;
      char* buf = (char*) kaAlloc(&swRest.kalloc, sz);
      kjFastRender(valueP, buf);
      v_compnd = buf;
    }
  }

  const char* obsAtIso = (observP != NULL && observP->type == KjString) ? observP->value.s : NULL;

  TimescaleConn* cP = timescaleConnGet(tenantP);
  if (cP == NULL) return TROE_ERR;
  timescaleConn = cP->conn;

  if (!entityHasRows(tenant, entityId))
  {
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return TROE_NOT_FOUND;
  }
  if (!instanceExists(tenant, entityId, attrName, instanceId))
  {
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return TROE_NOT_FOUND;
  }

  // $1=entity_id, $2=attr_name, $3=instance_id, $4=tenant,
  // $5=v_text, $6=v_number, $7=v_bool, $8=v_compound, $9=observed_at
  const char* paramV[9];
  paramV[0] = entityId;
  paramV[1] = attrName;
  paramV[2] = instanceId;
  paramV[3] = tenant;
  paramV[4] = v_text;
  paramV[5] = v_number;
  paramV[6] = v_bool;
  paramV[7] = v_compnd;
  paramV[8] = obsAtIso;

  // Reset all value columns then apply the supplied ones — a Modify that
  // only carries "value" cleanly clears any prior compound/bool of that row.
  // modified_at is bumped to the broker's request-start time per § 5.6.14.4
  // (faster than a postgres-side now() and consistent with the INSERT path).
  char tsExpr[64];
  timescaleNsToSqlTimestamp(swRest.requestStartTime, tsExpr, sizeof(tsExpr));

  char sql[1024];
  snprintf(sql, sizeof(sql),
    "UPDATE troe_attrs "
    "   SET v_text     = $5, "
    "       v_number   = $6::double precision, "
    "       v_bool     = $7::boolean, "
    "       v_compound = $8::jsonb, "
    "       observed_at = COALESCE($9::timestamptz, observed_at), "
    "       modified_at = %s "
    " WHERE entity_id = $1 AND attr_name = $2 AND instance_id = $3 AND tenant = $4",
    tsExpr);

  PGresult* res = PQexecParams(timescaleConn, sql, 9, NULL, paramV, NULL, NULL, 0);

  if (PQresultStatus(res) != PGRES_COMMAND_OK)
  {
    KT_E("timescale: instance UPDATE failed: %s", PQerrorMessage(timescaleConn));
    PQclear(res);
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return TROE_ERR;
  }
  PQclear(res);

  timescaleConn = NULL;
  timescaleConnRelease(cP);
  return TROE_OK;
}



// -----------------------------------------------------------------------------
//
// timescaleEntityTemporalInstanceDelete - § 5.6.15 / § 6.22.3.2.
//
int timescaleEntityTemporalInstanceDelete(Tenant* tenantP, const char* entityId,
                                          const char* attrName, const char* instanceId)
{
  if (entityId == NULL || attrName == NULL || instanceId == NULL)
    return TROE_ERR;

  const char* tenant = (tenantP != NULL) ? tenantP->name : "";

  TimescaleConn* cP = timescaleConnGet(tenantP);
  if (cP == NULL) return TROE_ERR;
  timescaleConn = cP->conn;

  if (!entityHasRows(tenant, entityId))
  {
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return TROE_NOT_FOUND;
  }
  if (!instanceExists(tenant, entityId, attrName, instanceId))
  {
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return TROE_NOT_FOUND;
  }

  const char* paramV[4] = { entityId, attrName, instanceId, tenant };
  PGresult* res = PQexecParams(timescaleConn,
    "DELETE FROM troe_attrs "
    " WHERE entity_id = $1 AND attr_name = $2 AND instance_id = $3 AND tenant = $4",
    4, NULL, paramV, NULL, NULL, 0);

  if (PQresultStatus(res) != PGRES_COMMAND_OK)
  {
    KT_E("timescale: instance DELETE failed: %s", PQerrorMessage(timescaleConn));
    PQclear(res);
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return TROE_ERR;
  }
  PQclear(res);

  timescaleConn = NULL;
  timescaleConnRelease(cP);
  return TROE_OK;
}



// -----------------------------------------------------------------------------
//
// timescaleEntityTemporalAttrsAdd - § 5.6.12 / § 6.20.3.1 — Add Attributes
// to Temporal Evolution of an Entity.
//
// The target entity must already exist in TRoE (otherwise → TROE_NOT_FOUND).
// No new entity-level row is written — only per-attribute instances.
//
int timescaleEntityTemporalAttrsAdd(Tenant* tenantP, const char* entityId, KjNode* rootP)
{
  if (entityId == NULL || rootP == NULL || rootP->type != KjObject)
    return TROE_ERR;

  const char* tenant = (tenantP != NULL) ? tenantP->name : "";

  TimescaleConn* cP = timescaleConnGet(tenantP);
  if (cP == NULL) return TROE_ERR;
  timescaleConn = cP->conn;

  if (!entityHasRows(tenant, entityId))
  {
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return TROE_NOT_FOUND;
  }

  // Look up entity_type from the most-recent troe_entities row so the
  // synthesised attr rows carry the right type (entity_type is part of
  // the troe_attrs schema, used for type-filtered queries).
  const char* idParamV[2] = { entityId, tenant };
  PGresult* tRes = PQexecParams(timescaleConn,
    "SELECT entity_type FROM troe_entities "
    "WHERE entity_id = $1 AND tenant = $2 ORDER BY modified_at DESC LIMIT 1",
    2, NULL, idParamV, NULL, NULL, 0);
  if (PQresultStatus(tRes) != PGRES_TUPLES_OK || PQntuples(tRes) == 0)
  {
    KT_E("timescale: entity_type lookup failed: %s", PQerrorMessage(timescaleConn));
    PQclear(tRes);
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return TROE_ERR;
  }
  const char* entityType = kaStrdup(&swRest.kalloc, PQgetvalue(tRes, 0, 0));
  PQclear(tRes);

  PGresult* beginR = PQexec(timescaleConn, "BEGIN");
  PQclear(beginR);

  int r = insertInstanceRows(tenantP, entityId, entityType, rootP, swRest.requestStartTime);
  if (r != TROE_OK)
  {
    PQclear(PQexec(timescaleConn, "ROLLBACK"));
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return r;
  }

  PQclear(PQexec(timescaleConn, "COMMIT"));
  timescaleConn = NULL;
  timescaleConnRelease(cP);
  return TROE_OK;
}

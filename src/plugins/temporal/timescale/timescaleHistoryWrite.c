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
// one transaction, holding the timescaleMutex throughout.
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
#include <string.h>                                       // strcmp, memset
#include <pthread.h>                                      // pthread_mutex_*
#include <libpq-fe.h>                                     // PG*

#include "ktrace/kTrace.h"                                // KT_E
#include "kjson/KjNode.h"                                 // KjNode
#include "kjson/kjLookup.h"                               // kjLookup
#include "kjson/kjBuilder.h"                              // kjObject, kjChildAdd
#include "kalloc/kaStrdup.h"                              // kaStrdup

#include "swRest/SwRestState.h"                           // swRest

#include "troe/TroeDriver.h"                              // TroeEvent, TROE_*

#include "temporal/timescale/timescaleGlobals.h"          // timescaleConn, timescaleMutex
#include "temporal/timescale/timescaleEvent.h"            // timescaleExec*Locked
#include "temporal/timescale/timescaleHistoryWrite.h"     // Own interface



// -----------------------------------------------------------------------------
//
// entityHasRows - quick existence check against troe_entities.
// Caller holds timescaleMutex.
//
static bool entityHasRows(const char* entityId)
{
  const char* paramV[1] = { entityId };
  PGresult* res = PQexecParams(timescaleConn,
    "SELECT 1 FROM troe_entities WHERE entity_id = $1 LIMIT 1",
    1, NULL, paramV, NULL, NULL, 0);
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
// Caller holds timescaleMutex.
//
static bool attrHasRows(const char* entityId, const char* attrName)
{
  const char* paramV[2] = { entityId, attrName };
  PGresult* res = PQexecParams(timescaleConn,
    "SELECT 1 FROM troe_attrs WHERE entity_id = $1 AND attr_name = $2 LIMIT 1",
    2, NULL, paramV, NULL, NULL, 0);
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
// timescaleEntityTemporalDelete - § 5.6.16 Delete Temporal Evolution.
//
int timescaleEntityTemporalDelete(Tenant* tenantP, const char* entityId)
{
  (void) tenantP;

  if (timescaleConn == NULL || entityId == NULL || entityId[0] == 0)
    return TROE_ERR;

  pthread_mutex_lock(&timescaleMutex);

  if (!entityHasRows(entityId))
  {
    pthread_mutex_unlock(&timescaleMutex);
    return TROE_NOT_FOUND;
  }

  const char* paramV[1] = { entityId };

  // Single statement per table — both DELETEs run in autocommit since
  // they're independent and either succeeding leaves the store in a
  // consistent (partial) state. For atomicity we wrap in BEGIN/COMMIT.
  PGresult* r = PQexec(timescaleConn, "BEGIN");
  PQclear(r);

  PGresult* attrsR = PQexecParams(timescaleConn,
    "DELETE FROM troe_attrs WHERE entity_id = $1",
    1, NULL, paramV, NULL, NULL, 0);
  if (PQresultStatus(attrsR) != PGRES_COMMAND_OK)
  {
    KT_E("timescale: troe_attrs DELETE failed: %s", PQerrorMessage(timescaleConn));
    PQclear(attrsR);
    PQclear(PQexec(timescaleConn, "ROLLBACK"));
    pthread_mutex_unlock(&timescaleMutex);
    return TROE_ERR;
  }
  PQclear(attrsR);

  PGresult* entR = PQexecParams(timescaleConn,
    "DELETE FROM troe_entities WHERE entity_id = $1",
    1, NULL, paramV, NULL, NULL, 0);
  if (PQresultStatus(entR) != PGRES_COMMAND_OK)
  {
    KT_E("timescale: troe_entities DELETE failed: %s", PQerrorMessage(timescaleConn));
    PQclear(entR);
    PQclear(PQexec(timescaleConn, "ROLLBACK"));
    pthread_mutex_unlock(&timescaleMutex);
    return TROE_ERR;
  }
  PQclear(entR);

  PQclear(PQexec(timescaleConn, "COMMIT"));
  pthread_mutex_unlock(&timescaleMutex);
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
  (void) tenantP;

  if (timescaleConn == NULL || entityId == NULL || attrName == NULL)
    return TROE_ERR;

  pthread_mutex_lock(&timescaleMutex);

  if (!entityHasRows(entityId))
  {
    pthread_mutex_unlock(&timescaleMutex);
    return TROE_NOT_FOUND;
  }
  if (!attrHasRows(entityId, attrName))
  {
    pthread_mutex_unlock(&timescaleMutex);
    return TROE_NOT_FOUND;
  }

  PGresult* res = NULL;
  if (deleteAll)
  {
    const char* paramV[2] = { entityId, attrName };
    res = PQexecParams(timescaleConn,
      "DELETE FROM troe_attrs WHERE entity_id = $1 AND attr_name = $2",
      2, NULL, paramV, NULL, NULL, 0);
  }
  else
  {
    const char* paramV[3] = { entityId, attrName, (datasetId != NULL) ? datasetId : "" };
    res = PQexecParams(timescaleConn,
      "DELETE FROM troe_attrs WHERE entity_id = $1 AND attr_name = $2 AND dataset_id = $3",
      3, NULL, paramV, NULL, NULL, 0);
  }

  if (PQresultStatus(res) != PGRES_COMMAND_OK)
  {
    KT_E("timescale: troe_attrs attr-delete failed: %s", PQerrorMessage(timescaleConn));
    PQclear(res);
    pthread_mutex_unlock(&timescaleMutex);
    return TROE_ERR;
  }
  PQclear(res);

  pthread_mutex_unlock(&timescaleMutex);
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
// (attribute, instance) into troe_attrs. Caller holds timescaleMutex and
// has begun a transaction.
//
// modAtNs0 = base modified-at (request start time). Successive instances
// receive +1 ms offsets so the PK stays unique under shared observedAt.
//
static int insertInstanceRows(Tenant* tenantP, const char* entityId,
                              const char* entityType, KjNode* rootP,
                              uint64_t modAtNs0)
{
  uint64_t nsOffset = 0;

  for (KjNode* attrP = rootP->value.firstChildP; attrP != NULL; attrP = attrP->next)
  {
    if (attrP->name == NULL)                         continue;
    if (attrP->name[0] == '@')                       continue;
    if (strcmp(attrP->name, "id")        == 0)       continue;
    if (strcmp(attrP->name, "type")      == 0)       continue;
    if (strcmp(attrP->name, "scope")     == 0)       continue;
    if (strcmp(attrP->name, "createdAt") == 0)       continue;
    if (strcmp(attrP->name, "modifiedAt") == 0)      continue;

    // EntityTemporal: each attribute is an array of instance objects.
    // Tolerate single-object form (some clients omit the array).
    if (attrP->type == KjArray)
    {
      // Walk the array carefully — instanceWrap stomps `next`. Capture
      // each child's `next` pointer before wrapping.
      KjNode* instP = attrP->value.firstChildP;
      while (instP != NULL)
      {
        KjNode* nextP = instP->next;

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
        instP = nextP;
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
  (void) tenantP;

  if (timescaleConn == NULL || rootP == NULL || rootP->type != KjObject)
    return TROE_ERR;

  KjNode* idP   = kjLookup(rootP, "id");
  KjNode* typeP = kjLookup(rootP, "type");
  if (idP == NULL || idP->type != KjString || idP->value.s[0] == 0)
    return TROE_ERR;
  if (typeP == NULL || typeP->type != KjString || typeP->value.s[0] == 0)
    return TROE_ERR;

  const char* entityId   = idP->value.s;
  const char* entityType = typeP->value.s;

  pthread_mutex_lock(&timescaleMutex);

  PGresult* beginR = PQexec(timescaleConn, "BEGIN");
  PQclear(beginR);

  // Entity row.
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
    pthread_mutex_unlock(&timescaleMutex);
    return r;
  }

  // Attr rows.
  r = insertInstanceRows(tenantP, entityId, entityType, rootP, swRest.requestStartTime);
  if (r != TROE_OK)
  {
    PQclear(PQexec(timescaleConn, "ROLLBACK"));
    pthread_mutex_unlock(&timescaleMutex);
    return r;
  }

  PQclear(PQexec(timescaleConn, "COMMIT"));
  pthread_mutex_unlock(&timescaleMutex);
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
  (void) tenantP;

  if (timescaleConn == NULL || entityId == NULL || rootP == NULL || rootP->type != KjObject)
    return TROE_ERR;

  pthread_mutex_lock(&timescaleMutex);

  if (!entityHasRows(entityId))
  {
    pthread_mutex_unlock(&timescaleMutex);
    return TROE_NOT_FOUND;
  }

  // Look up entity_type from the most-recent troe_entities row so the
  // synthesised attr rows carry the right type (entity_type is part of
  // the troe_attrs schema, used for type-filtered queries).
  const char* idParamV[1] = { entityId };
  PGresult* tRes = PQexecParams(timescaleConn,
    "SELECT entity_type FROM troe_entities "
    "WHERE entity_id = $1 ORDER BY modified_at DESC LIMIT 1",
    1, NULL, idParamV, NULL, NULL, 0);
  if (PQresultStatus(tRes) != PGRES_TUPLES_OK || PQntuples(tRes) == 0)
  {
    KT_E("timescale: entity_type lookup failed: %s", PQerrorMessage(timescaleConn));
    PQclear(tRes);
    pthread_mutex_unlock(&timescaleMutex);
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
    pthread_mutex_unlock(&timescaleMutex);
    return r;
  }

  PQclear(PQexec(timescaleConn, "COMMIT"));
  pthread_mutex_unlock(&timescaleMutex);
  return TROE_OK;
}

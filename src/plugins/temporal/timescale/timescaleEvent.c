//
// FILE            timescaleEvent.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Skeleton write path. v1 stores entity-level metadata + attribute
// metadata only; value extraction (v_text / v_number / v_geo / sub_attrs)
// lands in the next slice. Even at this level the plugin gives us:
//   - real rows in postgres for every write
//   - an "alive entity?" check via the entity-level rows
//   - a count + per-attr breakdown of what changed
// which is enough to wire functests against and to validate the
// dispatch path end-to-end.
//

#include <stddef.h>                                       // NULL
#include <stdio.h>                                        // snprintf
#include <pthread.h>                                      // pthread_mutex_lock
#include <libpq-fe.h>                                     // PG*

#include "ktrace/kTrace.h"                                // KT_E

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
// Postgres's to_timestamp(double precision) keeps sub-second precision.
//
static void nsToSqlTimestamp(uint64_t ns, char* buf, int bufSize)
{
  double secs = (double) ns / 1e9;
  snprintf(buf, bufSize, "to_timestamp(%.6f)", secs);
}



// -----------------------------------------------------------------------------
//
// execEntityInsert -
//
static int execEntityInsert(const TroeEvent* evP)
{
  char tsExpr[64];
  nsToSqlTimestamp(evP->modifiedAtNs, tsExpr, sizeof(tsExpr));

  // Use parameterised query for entity_id / type to avoid quoting issues.
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
// execAttrInsert - skeleton, no value extraction yet.
//
static int execAttrInsert(const TroeEvent* evP)
{
  char tsExpr[64];
  nsToSqlTimestamp(evP->modifiedAtNs, tsExpr, sizeof(tsExpr));

  const char* paramV[5];
  paramV[0] = evP->entityId   != NULL ? evP->entityId   : "";
  paramV[1] = evP->entityType != NULL ? evP->entityType : "";
  paramV[2] = evP->attrName   != NULL ? evP->attrName   : "";
  paramV[3] = evP->datasetId  != NULL ? evP->datasetId  : "";
  paramV[4] = opName(evP->op);

  char sql[512];
  snprintf(sql, sizeof(sql),
           "INSERT INTO troe_attrs (entity_id, entity_type, attr_name, dataset_id, modified_at, op) "
           "VALUES ($1, $2, $3, $4, %s, $5) "
           "ON CONFLICT (entity_id, attr_name, dataset_id, modified_at, op) DO NOTHING",
           tsExpr);

  PGresult* res = PQexecParams(timescaleConn, sql, 5, NULL, paramV, NULL, NULL, 0);
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
// timescaleEntityEvent -
//
int timescaleEntityEvent(const TroeEvent* evP)
{
  if (evP == NULL || timescaleConn == NULL) return TROE_ERR;

  pthread_mutex_lock(&timescaleMutex);
  int r = execEntityInsert(evP);
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
  int r = execAttrInsert(evP);
  pthread_mutex_unlock(&timescaleMutex);
  return r;
}



// -----------------------------------------------------------------------------
//
// timescaleEventList - drain a queue of events as one transaction.
//
// Faster than per-event commits when the request produced multiple
// events; a single INSERT-many would be even faster but per-row INSERTs
// inside one BEGIN/COMMIT keep the code simple while still amortising
// fsync.
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
      r = execAttrInsert(evP);
    else
      r = execEntityInsert(evP);

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

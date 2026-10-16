//
// FILE            timepoint.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The timepoint resources - see timepoint.h
//
// What differs from the Core API's retrieve and query, and only that:
//   - timeAt is mandatory (400 without it; an invalid one is refused where it is parsed)
//   - the entities are the TRoE's history at timeAt (troe.entityAtRetrieve / entityAtQuery): a TRoE
//     without them answers as for any temporal operation it lacks
//   - local only: a registration's source is not asked - the timepoint operations are not distributed (yet)
//   - not on a Snapshot (NGSILD-Snapshot): a Snapshot is frozen values, a timepoint the history's
//   - no automatic EntityMap: its later pages would read today's entities (getEntities.c)
//
// The timeline timeAt is on: TIMEPOINT_AXIS - system time (what the broker held at timeAt).
//
#include <stdbool.h>                                 // bool

#include "corRest/CorRestState.h"                    // corRest
#include "corTree/CorNode.h"                         // CorNode

#include "corNgsild/corNgsild.h"                     // corNgsild, ldError, LD_ERROR_*

#include "db/DbDriver.h"                             // DB_OK, DB_NOT_FOUND, DB_ERR
#include "db/DbQueryFilter.h"                        // DbQueryFilter
#include "db/Tenant.h"                               // Tenant
#include "troe/TroeDriver.h"                         // troe, TROE_OK, TROE_NOT_FOUND, TROE_AT_MODIFIED
#include "troe/troeNotAvailable.h"                   // troeNotAvailable

#include "serviceRoutines/ldSnapshotRead.h"          // ldSnapshotItemFromHeader
#include "serviceRoutines/getEntity.h"               // getEntity
#include "serviceRoutines/getEntities.h"             // getEntities
#if !COR_FEATURE_GEOQ
#include "serviceRoutines/geoNotInThisBuild.h"       // geoQueryRefused
#endif
#include "serviceRoutines/timepoint.h"               // Own interface



// -----------------------------------------------------------------------------
//
// TIMEPOINT_AXIS - the timeline timeAt is on: TROE_AT_MODIFIED, system time - the entity as the broker
// held it at timeAt. TROE_AT_OBSERVED is the other one (each attribute's last observation at or before
// timeAt). The spec text does not say yet; this is the one place that decides.
//
#define TIMEPOINT_AXIS  TROE_AT_MODIFIED



// -----------------------------------------------------------------------------
//
// timepointStart - what every timepoint resource checks before the Core API routine runs; false: answered
//
static bool timepointStart(void)
{
  if ((corNgsild.timeAt == NULL) || (corNgsild.timeAtNs == 0))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Missing timeAt", "timeAt is mandatory - the point in time the entities are as they were");
    return false;
  }

  if ((troe.entityAtRetrieve == NULL) || (troe.entityAtQuery == NULL))
  {
    troeNotAvailable("entity at a point in time");
    return false;
  }

  bool seen = false;
  ldSnapshotItemFromHeader(&seen);

  if (seen)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid request", "a timepoint request reads the history - not a Snapshot (NGSILD-Snapshot)");
    return false;
  }

  corNgsild.timepoint = true;
  corNgsild.local     = true;                        // the history is this broker's: no registration's source is asked

  return true;
}



// -----------------------------------------------------------------------------
//
// getEntityTimepoint - GET /temporal/timepoint/entities/{id}
//
bool getEntityTimepoint(void)
{
  if (timepointStart() == false)
    return true;

  return getEntity();
}



// -----------------------------------------------------------------------------
//
// getEntitiesTimepoint - GET /temporal/timepoint/entities
//
bool getEntitiesTimepoint(void)
{
  if (timepointStart() == false)
    return true;

  return getEntities();
}



// -----------------------------------------------------------------------------
//
// postEntityBatchQueryTimepoint - POST /temporal/timepoint/entityOperations/query
//
// timeAt in the body - a member of the Query (a temporalQ is refused): the route's payload check made the
// body the request's params, timeAt among them, before this routine runs.
//
bool postEntityBatchQueryTimepoint(void)
{
#if !COR_FEATURE_GEOQ
  if (geoQueryRefused())                              // the body's geoQ, now in the request's georel
    return true;
#endif

  if (timepointStart() == false)
    return true;

  return getEntities();
}



// -----------------------------------------------------------------------------
//
// timepointEntityRetrieve - the entity: the store's, or for a timepoint request the history's at timeAt
//
int timepointEntityRetrieve(Tenant* tenantP, const char* entityId, CorNode** entityPP)
{
  if (corNgsild.timepoint == false)
    return db.entityRetrieve(tenantP, entityId, entityPP);

  int r = troe.entityAtRetrieve(tenantP, entityId, corNgsild.timeAtNs, TIMEPOINT_AXIS, entityPP);

  return (r == TROE_OK) ? DB_OK : ((r == TROE_NOT_FOUND) ? DB_NOT_FOUND : DB_ERR);
}



// -----------------------------------------------------------------------------
//
// timepointEntityQuery - the entities: the store's, or for a timepoint request the history's at timeAt
//
int timepointEntityQuery(Tenant* tenantP, DbQueryFilter* filterP, CorNode** arrayPP)
{
  if (corNgsild.timepoint == false)
    return db.entityQuery(tenantP, filterP, arrayPP);

  int r = troe.entityAtQuery(tenantP, filterP, corNgsild.timeAtNs, TIMEPOINT_AXIS, arrayPP);

  return (r == TROE_OK) ? DB_OK : DB_ERR;
}

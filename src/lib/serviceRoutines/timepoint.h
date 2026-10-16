#ifndef SERVICE_ROUTINES_TIMEPOINT_H_
#define SERVICE_ROUTINES_TIMEPOINT_H_

//
// FILE            timepoint.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The timepoint resources (NGSI-LD v2.1): an entity, a query, AS IT WAS at timeAt - the Core API's
// retrieve and query, the entities read from the TRoE's history instead of the current state
//
//   GET  /temporal/timepoint/entities/{id}              getEntityTimepoint     -> getEntity
//   GET  /temporal/timepoint/entities                   getEntitiesTimepoint   -> getEntities
//   POST /temporal/timepoint/entityOperations/query     postEntityBatchQueryTimepoint -> getEntities (the body as POST query reads it)
//
// timeAt is mandatory; everything else is the Core API's - its parameters, its rendering, its errors.
// The routines that read entities ask timepointEntityRetrieve / timepointEntityQuery: the store's
// answer, or for a timepoint request (corNgsild.timepoint) the history's at timeAt.
//
#include "corTree/CorNode.h"                         // CorNode
#include "corNgsild/ldParams.h"                      // LD_PARAMS_GET_ENTITY, LD_PARAMS_GET_ENTITIES, LD_PARAM_*

#include "db/DbDriver.h"                             // db
#include "db/DbQueryFilter.h"                        // DbQueryFilter
#include "db/Tenant.h"                               // Tenant



#if COR_FEATURE_TIMEPOINT

//
// TIMEPOINT_PARAMS_* - the routes' URL parameters: the Core API's, and timeAt; without entityMap and join,
// whose answers would be today's entities
//
#define TIMEPOINT_PARAMS_GET_ENTITY    ((LD_PARAMS_GET_ENTITY   & ~(LD_PARAM_JOIN | LD_PARAM_JOIN_LEVEL)) | LD_PARAM_TIMEAT)
#define TIMEPOINT_PARAMS_GET_ENTITIES  ((LD_PARAMS_GET_ENTITIES & ~(LD_PARAM_JOIN | LD_PARAM_JOIN_LEVEL | LD_PARAM_ENTITY_MAP)) | LD_PARAM_TIMEAT)

extern bool getEntityTimepoint(void);
extern bool getEntitiesTimepoint(void);
extern bool postEntityBatchQueryTimepoint(void);

extern int  timepointEntityRetrieve(Tenant* tenantP, const char* entityId, CorNode** entityPP);
extern int  timepointEntityQuery(Tenant* tenantP, DbQueryFilter* filterP, CorNode** arrayPP);

#else

#define timepointEntityRetrieve(tenantP, entityId, entityPP)  db.entityRetrieve(tenantP, entityId, entityPP)
#define timepointEntityQuery(tenantP, filterP, arrayPP)       db.entityQuery(tenantP, filterP, arrayPP)

#endif

#endif  // SERVICE_ROUTINES_TIMEPOINT_H_

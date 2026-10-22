#ifndef TIMESCALE_TIMESCALEINDEXES_H_
#define TIMESCALE_TIMESCALEINDEXES_H_

//
// FILE            timescaleIndexes.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <libpq-fe.h>                                     // PGconn

#include "corAlloc/CorAlloc.h"                            // CorAlloc
#include "corTree/CorNode.h"                              // CorNode
#include "db/Tenant.h"                                    // Tenant
#include "troe/TroeDriver.h"                              // TroeQueryFilter

#include "temporal/timescale/timescalePool.h"             // TimescalePool



// -----------------------------------------------------------------------------
//
// timescaleIndexesInit - --troeDbIndexes's declarations: before the default tenant's pool is built
//
extern int timescaleIndexesInit(void);



// -----------------------------------------------------------------------------
//
// timescaleIndexesLoad - the admin API's declarations, read from the default tenant's database, and
// their indexes in it: once its pool is there
//
extern int timescaleIndexesLoad(void);



// -----------------------------------------------------------------------------
//
// timescaleIndexesApply - a tenant's declared indexes: the ones of the declarations that cover it
// created, every other cor_n_* / cor_t_* index of troe_attrs dropped - on the connection of its pool's
// build
//
extern void timescaleIndexesApply(PGconn* conn, Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// timescaleIndexesDiscover - the indexes of troe_attrs that serve a q value term, the declared ones and
// the ones made in the database, kept in the pool (poolP->indexesP)
//
extern void timescaleIndexesDiscover(PGconn* conn, TimescalePool* poolP);



// -----------------------------------------------------------------------------
//
// timescaleIndexesPoolFree - what timescaleIndexesDiscover kept in a pool
//
extern void timescaleIndexesPoolFree(TimescalePool* poolP);



// -----------------------------------------------------------------------------
//
// timescaleIndexesCandidates - the entities a query's q can match, from the indexes of its tenant
// (troeQTreeToCandidateSql, its window still to be put in): NULL if no index serves it
//
extern const char* timescaleIndexesCandidates(PGconn* conn, Tenant* tenantP, TroeQueryFilter* fP, CorAlloc* allocP);



// -----------------------------------------------------------------------------
//
// timescaleIndexList, timescaleIndexAdd, timescaleIndexDelete - TroeDriver.indexList, indexAdd, indexDelete
//
extern int timescaleIndexList(CorNode** arrayPP);
extern int timescaleIndexAdd(const char* attr, const char* type, const char* tenant, CorNode** declPP, char* errorText, int errorTextLen);
extern int timescaleIndexDelete(const char* id, char* errorText, int errorTextLen);

#endif  // TIMESCALE_TIMESCALEINDEXES_H_

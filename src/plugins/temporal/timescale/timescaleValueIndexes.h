#ifndef TIMESCALE_TIMESCALEVALUEINDEXES_H_
#define TIMESCALE_TIMESCALEVALUEINDEXES_H_

//
// FILE            timescaleValueIndexes.h
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
// timescaleValueIndexesInit - --troeValueIndexes's declarations: before the default tenant's pool is built
//
extern int timescaleValueIndexesInit(void);



// -----------------------------------------------------------------------------
//
// timescaleValueIndexesLoad - the admin API's declarations, read from the default tenant's database, and
// their indexes in it: once its pool is there
//
extern int timescaleValueIndexesLoad(void);



// -----------------------------------------------------------------------------
//
// timescaleValueIndexesClose - the declarations freed (timescaleClose, after the pools)
//
extern void timescaleValueIndexesClose(void);



// -----------------------------------------------------------------------------
//
// timescaleValueIndexesApply - a tenant's declared indexes: the ones of the declarations that cover it
// created, every other cor_n_* / cor_t_* index of troe_attrs dropped - on the connection of its pool's
// build
//
extern void timescaleValueIndexesApply(PGconn* conn, Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// timescaleValueIndexesDiscover - the indexes of troe_attrs that serve a q value term, the declared ones and
// the ones made in the database, kept in the pool (poolP->indexesP)
//
extern void timescaleValueIndexesDiscover(PGconn* conn, TimescalePool* poolP);



// -----------------------------------------------------------------------------
//
// timescaleValueIndexesPoolFree - what timescaleValueIndexesDiscover kept in a pool
//
extern void timescaleValueIndexesPoolFree(TimescalePool* poolP);



// -----------------------------------------------------------------------------
//
// timescaleValueIndexesCandidates - the entities a query's q can match, from the indexes of its tenant
// (troeQTreeToCandidateSql, its window still to be put in): NULL if no index serves it
//
extern const char* timescaleValueIndexesCandidates(PGconn* conn, Tenant* tenantP, TroeQueryFilter* fP, CorAlloc* allocP);



// -----------------------------------------------------------------------------
//
// timescaleValueIndexList, timescaleValueIndexAdd, timescaleValueIndexDelete - TroeDriver.indexList, indexAdd, indexDelete
//
extern int timescaleValueIndexList(CorNode** arrayPP);
extern int timescaleValueIndexAdd(const char* attr, const char* type, const char* tenant, CorNode** declPP, char* errorText, int errorTextLen);
extern int timescaleValueIndexDelete(const char* id, char* errorText, int errorTextLen);

#endif  // TIMESCALE_TIMESCALEVALUEINDEXES_H_

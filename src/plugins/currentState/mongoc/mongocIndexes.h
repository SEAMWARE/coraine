#ifndef MONGOC_MONGOCINDEXES_H_
#define MONGOC_MONGOCINDEXES_H_

//
// FILE            mongocIndexes.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <mongoc/mongoc.h>                           // mongoc_collection_t

#include "corTree/CorNode.h"                         // CorNode
#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// mongocIndexesInit - --dbValueIndexes's declarations and the admin API's, before any tenant is set up: -1 on
// an error (said why)
//
extern int mongocIndexesInit(void);



// -----------------------------------------------------------------------------
//
// mongocIndexesApply - a tenant's declared indexes: created, the ones no longer declared dropped
//
extern void mongocIndexesApply(Tenant* tenantP, mongoc_collection_t* collP);



// -----------------------------------------------------------------------------
//
// mongocIndexList, mongocIndexAdd, mongocIndexDelete - DbDriver.indexList, indexAdd, indexDelete
//
extern int mongocIndexList(CorNode** arrayPP);
extern int mongocIndexAdd(const char* attr, const char* type, const char* tenant, CorNode** declPP, char* errorText, int errorTextLen);
extern int mongocIndexDelete(const char* id, char* errorText, int errorTextLen);

#endif  // MONGOC_MONGOCINDEXES_H_

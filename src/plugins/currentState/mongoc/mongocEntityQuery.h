#ifndef MONGOC_MONGOCENTITYQUERY_H_
#define MONGOC_MONGOCENTITYQUERY_H_

//
// FILE            mongocEntityQuery.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include "kjson/KjNode.h"                            // KjNode

#include "db/DbQueryFilter.h"                          // DbQueryFilter
#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// mongocEntityQuery -
//
extern int mongocEntityQuery(Tenant* tenantP, DbQueryFilter* filterP, KjNode** arrayPP);

#endif  // MONGOC_MONGOCENTITYQUERY_H_

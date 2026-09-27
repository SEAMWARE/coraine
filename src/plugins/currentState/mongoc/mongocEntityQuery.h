#ifndef MONGOC_MONGOCENTITYQUERY_H_
#define MONGOC_MONGOCENTITYQUERY_H_

//
// FILE            mongocEntityQuery.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include "corTree/CorNode.h"                         // CorNode

#include "db/DbQueryFilter.h"                          // DbQueryFilter
#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// mongocEntityQuery -
//
extern int mongocEntityQuery(Tenant* tenantP, DbQueryFilter* filterP, CorNode** arrayPP);

#endif  // MONGOC_MONGOCENTITYQUERY_H_

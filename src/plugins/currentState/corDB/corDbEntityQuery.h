#ifndef CORDB_CORDBENTITYQUERY_H_
#define CORDB_CORDBENTITYQUERY_H_

//
// FILE            corDbEntityQuery.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/DbQueryFilter.h"                          // DbQueryFilter
#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// corDbEntityQuery -
//
extern int corDbEntityQuery(Tenant* tenantP, DbQueryFilter* filterP, KjNode** arrayPP);

#endif  // CORDB_CORDBENTITYQUERY_H_
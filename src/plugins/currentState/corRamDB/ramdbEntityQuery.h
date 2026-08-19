#ifndef CORRAMDB_RAMDBENTITYQUERY_H_
#define CORRAMDB_RAMDBENTITYQUERY_H_

//
// FILE            ramdbEntityQuery.h
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
// ramdbEntityQuery -
//
extern int ramdbEntityQuery(Tenant* tenantP, DbQueryFilter* filterP, KjNode** arrayPP);

#endif  // CORRAMDB_RAMDBENTITYQUERY_H_

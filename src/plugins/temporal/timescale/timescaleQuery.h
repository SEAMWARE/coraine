#ifndef TIMESCALE_TIMESCALEQUERY_H_
#define TIMESCALE_TIMESCALEQUERY_H_

//
// FILE            timescaleQuery.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Read path. v1 retrieves the full temporal evolution of one entity
// — no time-window / q / pick filtering yet. The result is a
// TemporalEntity tree: { id, type, <attr>: [<instance>, ...], ... }.
//

#include "troe/TroeDriver.h"                              // TroeQueryFilter, TROE_*
#include "kjson/KjNode.h"                                 // KjNode
#include "db/Tenant.h"                                    // Tenant


extern int timescaleEntityTemporalRetrieve(Tenant* tenantP, const char* entityId,
                                           TroeQueryFilter* fP, KjNode** resultPP,
                                           TroeRangeInfo* rangeOut);

extern int timescaleEntityTemporalQuery(Tenant* tenantP, TroeQueryFilter* fP,
                                        KjNode** resultPP, TroeRangeInfo* rangeOut);

#endif  // TIMESCALE_TIMESCALEQUERY_H_

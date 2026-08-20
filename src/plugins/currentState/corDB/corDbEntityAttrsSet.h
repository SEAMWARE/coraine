#ifndef CORDB_CORDBENTITYATTRSSET_H_
#define CORDB_CORDBENTITYATTRSSET_H_

//
// FILE            corDbEntityAttrsSet.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Thin corDb wrapper around corNgsild's ldEntityAttrsSet — locates the
// live entity in the tenant store and applies the fragment in place.
//

#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // uint64_t

#include "kjson/KjNode.h"                             // KjNode
#include "corNgsild/ldEntityMerge.h"                   // LdMergeReport

#include "db/Tenant.h"                                 // Tenant



extern int corDbEntityAttrsSet(Tenant* tenantP, const char* entityId,
                               KjNode* fragmentDb, bool overwriteScope,
                               uint64_t ts, LdMergeReport* reportP);

#endif  // CORDB_CORDBENTITYATTRSSET_H_
#ifndef MONGOC_MONGOCENTITYATTRSSET_H_
#define MONGOC_MONGOCENTITYATTRSSET_H_

//
// FILE            mongocEntityAttrsSet.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                                   // bool
#include <stdint.h>                                    // uint64_t

#include "kjson/KjNode.h"                              // KjNode
#include "corNgsild/ldEntityMerge.h"                    // LdMergeReport

#include "db/Tenant.h"                                 // Tenant



extern int mongocEntityAttrsSet(Tenant*        tenantP,
                                const char*    entityId,
                                KjNode*        fragmentDb,
                                bool           overwriteScope,
                                uint64_t       ts,
                                LdMergeReport* reportP);

#endif  // MONGOC_MONGOCENTITYATTRSSET_H_

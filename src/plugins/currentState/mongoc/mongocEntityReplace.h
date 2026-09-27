#ifndef MONGOC_ENTITY_REPLACE_H_
#define MONGOC_ENTITY_REPLACE_H_

//
// FILE            mongocEntityReplace.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include "corTree/CorNode.h"                          // CorNode
#include "db/Tenant.h"                                // Tenant



// -----------------------------------------------------------------------------
//
// mongocEntityReplace -
//
// Atomic findAndModify-based replace-by-_id. Returns the pre-replacement
// document via *oldEntityPP on success.
//
//   DB_OK         — replaced; *oldEntityPP is populated (allocator: corRest.kalloc)
//   DB_NOT_FOUND  — no document matched; collection unchanged
//   DB_ERR        — driver/server error
//
int mongocEntityReplace(Tenant*      tenantP,
                        const char*  entityId,
                        CorNode*     newEntityP,
                        CorNode**    oldEntityPP);

#endif  // MONGOC_ENTITY_REPLACE_H_

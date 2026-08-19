#ifndef MONGOC_ENTITY_REPLACE_H_
#define MONGOC_ENTITY_REPLACE_H_

//
// FILE            mongocEntityReplace.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include "kjson/KjNode.h"                             // KjNode
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
                        KjNode*      newEntityP,
                        KjNode**     oldEntityPP);

#endif  // MONGOC_ENTITY_REPLACE_H_

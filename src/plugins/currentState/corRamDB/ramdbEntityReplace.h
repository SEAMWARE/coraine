#ifndef RAMDB_ENTITY_REPLACE_H_
#define RAMDB_ENTITY_REPLACE_H_

//
// FILE            ramdbEntityReplace.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include "kjson/KjNode.h"                             // KjNode
#include "db/Tenant.h"                                // Tenant



// -----------------------------------------------------------------------------
//
// ramdbEntityReplace -
//
// Atomic replace-by-id: detaches the stored entity tree, grafts a cloned new
// tree in its place, and hands back the detached old tree via *oldEntityPP.
//
// Returns:
//   DB_OK         — replaced; *oldEntityPP is set (caller may read and kjFree)
//   DB_NOT_FOUND  — no entity with this id; store unchanged
//   DB_ERR        — clone failure; store unchanged
//
int ramdbEntityReplace(Tenant*      tenantP,
                       const char*  entityId,
                       KjNode*      newEntityP,
                       KjNode**     oldEntityPP);

#endif  // RAMDB_ENTITY_REPLACE_H_

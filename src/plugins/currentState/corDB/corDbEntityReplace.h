#ifndef CORDB_CORDBENTITYREPLACE_H_
#define CORDB_CORDBENTITYREPLACE_H_

//
// FILE            corDbEntityReplace.h
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
// corDbEntityReplace -
//
// Atomic replace-by-id: detaches the stored entity tree, grafts a cloned new
// tree in its place, and hands back the detached old tree via *oldEntityPP.
//
// Returns:
//   DB_OK         — replaced; *oldEntityPP is set (caller may read and kjFree)
//   DB_NOT_FOUND  — no entity with this id; store unchanged
//   DB_ERR        — clone failure; store unchanged
//
int corDbEntityReplace(Tenant*      tenantP,
                       const char*  entityId,
                       KjNode*      newEntityP,
                       KjNode**     oldEntityPP);

#endif  // CORDB_CORDBENTITYREPLACE_H_
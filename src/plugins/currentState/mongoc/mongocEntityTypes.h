#ifndef MONGOC_MONGOCENTITYTYPES_H_
#define MONGOC_MONGOCENTITYTYPES_H_

//
// FILE            mongocEntityTypes.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                         // CorNode
#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// mongocEntityTypes - an entity's "type" member alone (DbDriver.entityTypes)
//
extern int mongocEntityTypes(Tenant* tenantP, const char* entityId, CorNode** typePP);

#endif  // MONGOC_MONGOCENTITYTYPES_H_

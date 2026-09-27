#ifndef MONGOC_MONGOCENTITYCREATE_H_
#define MONGOC_MONGOCENTITYCREATE_H_

//
// FILE            mongocEntityCreate.h
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
// mongocEntityCreate -
//
extern int mongocEntityCreate(Tenant* tenantP, const char* entityId, CorNode* entityP);

#endif  // MONGOC_MONGOCENTITYCREATE_H_

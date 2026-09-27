#ifndef CORDB_CORDBENTITYCREATE_H_
#define CORDB_CORDBENTITYCREATE_H_

//
// FILE            corDbEntityCreate.h
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
// corDbEntityCreate -
//
extern int corDbEntityCreate(Tenant* tenantP, const char* entityId, CorNode* entityP);

#endif  // CORDB_CORDBENTITYCREATE_H_
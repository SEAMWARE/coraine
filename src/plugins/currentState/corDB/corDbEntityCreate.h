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
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// corDbEntityCreate -
//
extern int corDbEntityCreate(Tenant* tenantP, const char* entityId, KjNode* entityP);

#endif  // CORDB_CORDBENTITYCREATE_H_
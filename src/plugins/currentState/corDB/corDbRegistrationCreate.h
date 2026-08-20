#ifndef CORDB_CORDBREGISTRATIONCREATE_H_
#define CORDB_CORDBREGISTRATIONCREATE_H_

//
// FILE            corDbRegistrationCreate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant

extern int corDbRegistrationCreate(Tenant* tenantP, const char* regId, KjNode* regP);

#endif  // CORDB_CORDBREGISTRATIONCREATE_H_
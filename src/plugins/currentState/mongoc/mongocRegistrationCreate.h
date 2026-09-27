#ifndef MONGOC_MONGOCREGISTRATIONCREATE_H_
#define MONGOC_MONGOCREGISTRATIONCREATE_H_

//
// FILE            mongocRegistrationCreate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                         // CorNode

#include "db/Tenant.h"                               // Tenant

extern int mongocRegistrationCreate(Tenant* tenantP, const char* regId, CorNode* regP);

#endif  // MONGOC_MONGOCREGISTRATIONCREATE_H_

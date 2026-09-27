#ifndef CORDB_CORDBREGISTRATIONUPDATE_H_
#define CORDB_CORDBREGISTRATIONUPDATE_H_

//
// FILE            corDbRegistrationUpdate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                         // CorNode

#include "db/Tenant.h"                               // Tenant

extern int corDbRegistrationUpdate(Tenant* tenantP, const char* regId, CorNode* fragmentP);

#endif  // CORDB_CORDBREGISTRATIONUPDATE_H_
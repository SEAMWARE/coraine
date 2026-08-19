#ifndef MONGOC_MONGOCTENANTSETUP_H_
#define MONGOC_MONGOCTENANTSETUP_H_

//
// FILE            mongocTenantSetup.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// mongocTenantSetup - create indexes (type + geo) for a tenant's database
//
extern int mongocTenantSetup(Tenant* tenantP);

#endif  // MONGOC_MONGOCTENANTSETUP_H_

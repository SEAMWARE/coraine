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
// mongocTenantSetup - the storage format checked and recorded, the indexes created; -1: the database is
// in a newer storage format than this build knows - the tenant is not to be used
//
extern int mongocTenantSetup(Tenant* tenantP);

#endif  // MONGOC_MONGOCTENANTSETUP_H_

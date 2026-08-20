#ifndef CORDB_CORDBSTORE_H_
#define CORDB_CORDBSTORE_H_

//
// FILE            corDbStore.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// corDbTenantStore - return (or create) the per-tenant KjNode tree
//
// The tree is an object with three array children:
//   { "entities": [...], "subscriptions": [...], "registrations": [...] }
//
// Stored in tenant->pluginData.  Created lazily on first access.
//
extern KjNode* corDbTenantStore(Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// corDbEntities - return the "entities" array for a tenant
//
extern KjNode* corDbEntities(Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// corDbSubscriptions - return the "subscriptions" array for a tenant
//
extern KjNode* corDbSubscriptions(Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// corDbRegistrations - return the "registrations" array for a tenant
//
extern KjNode* corDbRegistrations(Tenant* tenantP);

#endif  // CORDB_CORDBSTORE_H_
#ifndef CORRAMDB_RAMDBSTORE_H_
#define CORRAMDB_RAMDBSTORE_H_

//
// FILE            ramdbStore.h
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
// ramdbTenantStore - return (or create) the per-tenant KjNode tree
//
// The tree is an object with three array children:
//   { "entities": [...], "subscriptions": [...], "registrations": [...] }
//
// Stored in tenant->pluginData.  Created lazily on first access.
//
extern KjNode* ramdbTenantStore(Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// ramdbEntities - return the "entities" array for a tenant
//
extern KjNode* ramdbEntities(Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// ramdbSubscriptions - return the "subscriptions" array for a tenant
//
extern KjNode* ramdbSubscriptions(Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// ramdbRegistrations - return the "registrations" array for a tenant
//
extern KjNode* ramdbRegistrations(Tenant* tenantP);

#endif  // CORRAMDB_RAMDBSTORE_H_

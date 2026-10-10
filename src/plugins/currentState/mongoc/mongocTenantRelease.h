// SPDX-License-Identifier: Apache-2.0
#ifndef PLUGINS_MONGOC_MONGOCTENANTRELEASE_H_
#define PLUGINS_MONGOC_MONGOCTENANTRELEASE_H_

#include "db/Tenant.h"                               // Tenant

//
// mongocTenantRelease - what mongoc hung on a tenant (its geo index cache), freed right before the broker
// frees the Tenant (DbDriver.h, tenantRelease). The database is not touched. DB_OK.
//
extern int mongocTenantRelease(Tenant* tenantP);

#endif  // PLUGINS_MONGOC_MONGOCTENANTRELEASE_H_

// SPDX-License-Identifier: Apache-2.0
#ifndef PLUGINS_MONGOC_MONGOCSNAPSHOTDELETE_H_
#define PLUGINS_MONGOC_MONGOCSNAPSHOTDELETE_H_

#include "db/Tenant.h"                               // Tenant

extern int mongocSnapshotDelete(Tenant* tenantP, const char* snapId);

#endif  // PLUGINS_MONGOC_MONGOCSNAPSHOTDELETE_H_

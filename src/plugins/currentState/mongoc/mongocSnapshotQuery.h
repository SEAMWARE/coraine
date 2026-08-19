// SPDX-License-Identifier: Apache-2.0
#ifndef PLUGINS_MONGOC_MONGOCSNAPSHOTQUERY_H_
#define PLUGINS_MONGOC_MONGOCSNAPSHOTQUERY_H_

#include "kjson/KjNode.h"                            // KjNode
#include "db/Tenant.h"                               // Tenant

extern int mongocSnapshotQuery(Tenant* tenantP, KjNode** arrayPP);

#endif  // PLUGINS_MONGOC_MONGOCSNAPSHOTQUERY_H_

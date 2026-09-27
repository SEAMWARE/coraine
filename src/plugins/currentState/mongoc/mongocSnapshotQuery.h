// SPDX-License-Identifier: Apache-2.0
#ifndef PLUGINS_MONGOC_MONGOCSNAPSHOTQUERY_H_
#define PLUGINS_MONGOC_MONGOCSNAPSHOTQUERY_H_

#include "corTree/CorNode.h"                         // CorNode
#include "db/Tenant.h"                               // Tenant

extern int mongocSnapshotQuery(Tenant* tenantP, CorNode** arrayPP);

#endif  // PLUGINS_MONGOC_MONGOCSNAPSHOTQUERY_H_

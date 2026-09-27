// SPDX-License-Identifier: Apache-2.0
#ifndef PLUGINS_MONGOC_MONGOCSNAPSHOTUPDATE_H_
#define PLUGINS_MONGOC_MONGOCSNAPSHOTUPDATE_H_

#include "corTree/CorNode.h"                         // CorNode
#include "db/Tenant.h"                               // Tenant

extern int mongocSnapshotUpdate(Tenant* tenantP, const char* snapId, CorNode* fragmentP);

#endif  // PLUGINS_MONGOC_MONGOCSNAPSHOTUPDATE_H_

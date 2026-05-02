#ifndef PLUGINS_MONGOC_MONGOCSNAPSHOTUPDATE_H_
#define PLUGINS_MONGOC_MONGOCSNAPSHOTUPDATE_H_

#include "kjson/KjNode.h"                            // KjNode
#include "db/Tenant.h"                               // Tenant

extern int mongocSnapshotUpdate(Tenant* tenantP, const char* snapId, KjNode* fragmentP);

#endif  // PLUGINS_MONGOC_MONGOCSNAPSHOTUPDATE_H_

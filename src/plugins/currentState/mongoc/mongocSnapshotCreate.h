#ifndef PLUGINS_MONGOC_MONGOCSNAPSHOTCREATE_H_
#define PLUGINS_MONGOC_MONGOCSNAPSHOTCREATE_H_

//
// FILE            mongocSnapshotCreate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                            // KjNode
#include "db/Tenant.h"                               // Tenant

extern int mongocSnapshotCreate(Tenant* tenantP, const char* snapId, KjNode* snapP);

#endif  // PLUGINS_MONGOC_MONGOCSNAPSHOTCREATE_H_

#ifndef PLUGINS_MONGOC_MONGOCSNAPSHOTCREATE_H_
#define PLUGINS_MONGOC_MONGOCSNAPSHOTCREATE_H_

//
// FILE            mongocSnapshotCreate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                         // CorNode
#include "db/Tenant.h"                               // Tenant

extern int mongocSnapshotCreate(Tenant* tenantP, const char* snapId, CorNode* snapP);

#endif  // PLUGINS_MONGOC_MONGOCSNAPSHOTCREATE_H_

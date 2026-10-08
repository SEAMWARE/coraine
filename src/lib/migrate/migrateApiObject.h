#ifndef MIGRATE_MIGRATEAPIOBJECT_H_
#define MIGRATE_MIGRATEAPIOBJECT_H_

//
// FILE            migrateApiObject.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                      // bool

#include "corTree/CorNode.h"                              // CorNode

#include "db/Tenant.h"                                    // Tenant
#include "migrate/MigrateState.h"                         // MigrateState



// -----------------------------------------------------------------------------
//
// migrateSubscription - import one Subscription, keeping its id, createdAt, modifiedAt and counters
//
extern bool migrateSubscription(MigrateState* msP, Tenant* tenantP, CorNode* subP);



// -----------------------------------------------------------------------------
//
// migrateRegistration - import one Context Source Registration, keeping its id, createdAt, modifiedAt
//
extern bool migrateRegistration(MigrateState* msP, Tenant* tenantP, CorNode* regP);

#endif  // MIGRATE_MIGRATEAPIOBJECT_H_

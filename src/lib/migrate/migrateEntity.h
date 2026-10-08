#ifndef MIGRATE_MIGRATEENTITY_H_
#define MIGRATE_MIGRATEENTITY_H_

//
// FILE            migrateEntity.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdint.h>                                       // int64_t
#include <stdbool.h>                                      // bool

#include "corTree/CorNode.h"                              // CorNode

#include "db/Tenant.h"                                    // Tenant
#include "migrate/MigrateState.h"                         // MigrateState, MigrateKind



// -----------------------------------------------------------------------------
//
// migrateEntityToDbModel - an Entity of the stream (expanded NGSI-LD, normalized, with its system
// timestamps), converted IN PLACE to the broker's DB model by the broker's own code, keeping the
// source's createdAt/modifiedAt on the entity, every attribute instance and every Sub-Attribute
//
extern bool migrateEntityToDbModel(MigrateState* msP, MigrateKind kind, CorNode* entityP, int64_t* createdAtP, int64_t* modifiedAtP);



// -----------------------------------------------------------------------------
//
// migrateEntity - import one Entity into the current-state store (db.entityCreate)
//
extern bool migrateEntity(MigrateState* msP, Tenant* tenantP, CorNode* entityP);

#endif  // MIGRATE_MIGRATEENTITY_H_

#ifndef CORRAMDB_RAMDBENTITYRETRIEVE_H_
#define CORRAMDB_RAMDBENTITYRETRIEVE_H_

//
// FILE            ramdbEntityRetrieve.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// ramdbEntityRetrieve -
//
extern int ramdbEntityRetrieve(Tenant* tenantP, const char* entityId, KjNode** entityPP);

#endif  // CORRAMDB_RAMDBENTITYRETRIEVE_H_

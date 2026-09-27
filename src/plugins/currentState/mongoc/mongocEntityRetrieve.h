#ifndef MONGOC_MONGOCENTITYRETRIEVE_H_
#define MONGOC_MONGOCENTITYRETRIEVE_H_

//
// FILE            mongocEntityRetrieve.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include "corTree/CorNode.h"                         // CorNode

#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// mongocEntityRetrieve -
//
extern int mongocEntityRetrieve(Tenant* tenantP, const char* entityId, CorNode** entityPP);

#endif  // MONGOC_MONGOCENTITYRETRIEVE_H_

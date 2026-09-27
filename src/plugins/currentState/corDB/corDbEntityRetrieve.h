#ifndef CORDB_CORDBENTITYRETRIEVE_H_
#define CORDB_CORDBENTITYRETRIEVE_H_

//
// FILE            corDbEntityRetrieve.h
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
// corDbEntityRetrieve -
//
extern int corDbEntityRetrieve(Tenant* tenantP, const char* entityId, CorNode** entityPP);

#endif  // CORDB_CORDBENTITYRETRIEVE_H_
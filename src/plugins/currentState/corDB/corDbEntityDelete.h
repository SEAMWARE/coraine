#ifndef CORDB_CORDBENTITYDELETE_H_
#define CORDB_CORDBENTITYDELETE_H_

//
// FILE            corDbEntityDelete.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// corDbEntityDelete -
//
extern int corDbEntityDelete(Tenant* tenantP, const char* entityId);

#endif  // CORDB_CORDBENTITYDELETE_H_
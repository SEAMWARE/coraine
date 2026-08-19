#ifndef MONGOC_MONGOCENTITYDELETE_H_
#define MONGOC_MONGOCENTITYDELETE_H_

//
// FILE            mongocEntityDelete.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// mongocEntityDelete -
//
extern int mongocEntityDelete(Tenant* tenantP, const char* entityId);

#endif  // MONGOC_MONGOCENTITYDELETE_H_

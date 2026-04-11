#ifndef SWRAMDB_RAMDBENTITYDELETE_H_
#define SWRAMDB_RAMDBENTITYDELETE_H_

//
// FILE            ramdbEntityDelete.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// ramdbEntityDelete -
//
extern int ramdbEntityDelete(Tenant* tenantP, const char* entityId);

#endif  // SWRAMDB_RAMDBENTITYDELETE_H_

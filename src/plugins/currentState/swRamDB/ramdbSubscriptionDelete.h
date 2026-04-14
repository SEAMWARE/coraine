#ifndef SWRAMDB_RAMDBSUBSCRIPTIONDELETE_H_
#define SWRAMDB_RAMDBSUBSCRIPTIONDELETE_H_

//
// FILE            ramdbSubscriptionDelete.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// ramdbSubscriptionDelete -
//
extern int ramdbSubscriptionDelete(Tenant* tenantP, const char* subId);

#endif  // SWRAMDB_RAMDBSUBSCRIPTIONDELETE_H_

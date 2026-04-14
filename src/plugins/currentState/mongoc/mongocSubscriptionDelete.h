#ifndef MONGOC_MONGOCSUBSCRIPTIONDELETE_H_
#define MONGOC_MONGOCSUBSCRIPTIONDELETE_H_

//
// FILE            mongocSubscriptionDelete.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include "db/Tenant.h"                               // Tenant

extern int mongocSubscriptionDelete(Tenant* tenantP, const char* subId);

#endif  // MONGOC_MONGOCSUBSCRIPTIONDELETE_H_

#ifndef MONGOC_TYPE_LIST_H_
#define MONGOC_TYPE_LIST_H_
//
// FILE            mongocTypeList.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdbool.h>
#include "kjson/KjNode.h"
#include "db/Tenant.h"

extern int mongocTypeList(Tenant* tenantP, bool details, KjNode** arrayPP);

#endif

#ifndef MONGOC_ATTR_LIST_H_
#define MONGOC_ATTR_LIST_H_
//
// FILE            mongocAttrList.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdbool.h>
#include "kjson/KjNode.h"
#include "db/Tenant.h"

extern int mongocAttrList(Tenant* tenantP, bool details, KjNode** arrayPP);

#endif

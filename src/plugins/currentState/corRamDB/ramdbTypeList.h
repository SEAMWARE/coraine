#ifndef CORAINE_RAMDB_TYPE_LIST_H_
#define CORAINE_RAMDB_TYPE_LIST_H_
//
// FILE            ramdbTypeList.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>
#include "kjson/KjNode.h"
#include "db/Tenant.h"

extern int ramdbTypeList(Tenant* tenantP, bool details, KjNode** arrayPP);

#endif

#ifndef CORAINE_RAMDB_ATTR_LIST_H_
#define CORAINE_RAMDB_ATTR_LIST_H_
//
// FILE            ramdbAttrList.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>
#include "kjson/KjNode.h"
#include "db/Tenant.h"

extern int ramdbAttrList(Tenant* tenantP, bool details, KjNode** arrayPP);

#endif

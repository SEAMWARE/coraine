#ifndef MONGOC_ATTR_LIST_H_
#define MONGOC_ATTR_LIST_H_
//
// FILE            mongocAttrList.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>
#include "corTree/CorNode.h"
#include "db/Tenant.h"

extern int mongocAttrList(Tenant* tenantP, bool details, CorNode** arrayPP);

#endif

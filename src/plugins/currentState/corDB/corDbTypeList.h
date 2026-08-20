#ifndef CORDB_CORDBTYPELIST_H_
#define CORDB_CORDBTYPELIST_H_
//
// FILE            corDbTypeList.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>
#include "kjson/KjNode.h"
#include "db/Tenant.h"

extern int corDbTypeList(Tenant* tenantP, bool details, KjNode** arrayPP);

#endif

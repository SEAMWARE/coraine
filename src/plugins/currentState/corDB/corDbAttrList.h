#ifndef CORDB_CORDBATTRLIST_H_
#define CORDB_CORDBATTRLIST_H_
//
// FILE            corDbAttrList.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>
#include "kjson/KjNode.h"
#include "db/Tenant.h"

extern int corDbAttrList(Tenant* tenantP, bool details, KjNode** arrayPP);

#endif

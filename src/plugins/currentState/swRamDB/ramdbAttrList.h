#ifndef SWBROKER_RAMDB_ATTR_LIST_H_
#define SWBROKER_RAMDB_ATTR_LIST_H_
//
// FILE            ramdbAttrList.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdbool.h>
#include "kjson/KjNode.h"
#include "db/Tenant.h"

extern int ramdbAttrList(Tenant* tenantP, bool details, KjNode** arrayPP);

#endif

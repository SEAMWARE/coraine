#ifndef SRC_LIB_SERVICEROUTINES_DISTSUBPERSIST_H_
#define SRC_LIB_SERVICEROUTINES_DISTSUBPERSIST_H_
//
// FILE            distSubPersist.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corNgsild/LdSubCache.h"                     // LdSubCacheItem



// -----------------------------------------------------------------------------
//
// distSubPersist - a subscription's subordinate mapping into its document
//
// The LdDistSubPersistFunc of every ldDistSub* call (corNgsild calls it whenever an itemP->subordinateP list
// changes): {_subordinates, _subordinateRunNo} merged onto the subscription's document, so the mapping survives a
// restart. userData is the Tenant.
//
extern void distSubPersist(LdSubCacheItem* itemP, void* userData);

#endif  // SRC_LIB_SERVICEROUTINES_DISTSUBPERSIST_H_

#ifndef CURRENTSTATE_MONGOC_MONGOCVERSION_H_
#define CURRENTSTATE_MONGOC_MONGOCVERSION_H_

//
// FILE            mongocVersion.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include "kalloc/KAlloc.h"                           // KAlloc
#include "kjson/KjNode.h"                            // KjNode



// -----------------------------------------------------------------------------
//
// mongocServerVersionGet - query MongoDB server version (call after init)
//
extern int mongocServerVersionGet(void);



// -----------------------------------------------------------------------------
//
// mongocVersionInfo - add version entries to root object
//
extern void mongocVersionInfo(KAlloc* allocP, KjNode* root);

#endif  // CURRENTSTATE_MONGOC_MONGOCVERSION_H_

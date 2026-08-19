#ifndef CREATE_ENTITY_MAP_H
#define CREATE_ENTITY_MAP_H

//
// FILE            createEntityMap.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                              // bool



// -----------------------------------------------------------------------------
//
// createEntityMap - GET /entityMaps (§ 6.34.3.1). POST /entityMaps is
// handled by postEntityMap which translates the Query body and delegates
// here.
//
extern bool createEntityMap(void);

#endif  // CREATE_ENTITY_MAP_H

#ifndef POST_ENTITY_MAP_H
#define POST_ENTITY_MAP_H

//
// FILE            postEntityMap.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                              // bool



// -----------------------------------------------------------------------------
//
// postEntityMap - POST /entityMaps (§ 6.34.3.2). Accepts a Query body
// (§ 5.2.23), translates to internal query state, delegates to
// createEntityMap.
//
extern bool postEntityMap(void);

#endif  // POST_ENTITY_MAP_H

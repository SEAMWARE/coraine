//
// FILE            getBridges.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#ifndef GET_BRIDGES_H
#define GET_BRIDGES_H

#include <stdbool.h>                              // bool



// -----------------------------------------------------------------------------
//
// getBridges -
//
extern bool getBridges(void);



// -----------------------------------------------------------------------------
//
// bridgeLoaded - is a bridge plugin of this name loaded?
//
extern bool bridgeLoaded(const char* bridgeName);

#endif  // GET_BRIDGES_H

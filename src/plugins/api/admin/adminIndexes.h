#ifndef ADMIN_ADMININDEXES_H_
#define ADMIN_ADMININDEXES_H_

//
// FILE            adminIndexes.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                              // bool



// -----------------------------------------------------------------------------
//
// adminGetIndexes, adminPostIndexes, adminDeleteIndex - /admin/indexes; adminGetTroeIndexes, adminPostTroeIndexes,
// adminDeleteTroeIndex - /admin/troeIndexes (adminIndexes.c)
//
extern bool adminGetIndexes(void);
extern bool adminPostIndexes(void);
extern bool adminDeleteIndex(void);
extern bool adminGetTroeIndexes(void);
extern bool adminPostTroeIndexes(void);
extern bool adminDeleteTroeIndex(void);

#endif  // ADMIN_ADMININDEXES_H_

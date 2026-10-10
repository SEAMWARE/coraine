#ifndef MONGOC_MONGOCSTORAGEFORMAT_H_
#define MONGOC_MONGOCSTORAGEFORMAT_H_

//
// FILE            mongocStorageFormat.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                   // bool



// -----------------------------------------------------------------------------
//
// mongocStorageFormat - the storage format of a database checked (and, with 'write', recorded)
//
// 0: a format this build reads - with 'write', upgraded to and recorded as this build's own (a
// database with none recorded is new, or of coraine 0.4.x or earlier). -1: a newer format than this
// build knows, or an error - said why, and the database is not to be used.
//
extern int mongocStorageFormat(const char* dbName, bool write);

#endif  // MONGOC_MONGOCSTORAGEFORMAT_H_
